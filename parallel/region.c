#include "region.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/*********************************************************************************************
 Initialisation
 *********************************************************************************************/
void region_new(t_region *region, int n_regions, int nx[2], int id, int n_spec, t_species *spec,
		float box[], float dt, t_region *prev_region)
{
	region->id = id;
	region->prev = prev_region;

	region->limits_y[0] = floor((float) id * nx[1] / n_regions);
	region->limits_y[1] = floor((float) (id + 1) * nx[1] / n_regions);
	region->nx[0] = nx[0];
	region->nx[1] = region->limits_y[1] - region->limits_y[0];

	// Initialise particles in the region
	t_particle_vector *restrict particles;

	region->n_species = n_spec;
	region->species = (t_species*) malloc(n_spec * sizeof(t_species));
	assert(region->species);

	for (int n = 0; n < n_spec; ++n)
	{
		spec_new(&region->species[n], spec[n].name, spec[n].m_q, spec[n].ppc, spec[n].ufl, spec[n].uth,
				spec[n].nx, spec[n].box, spec[n].dt, &spec[n].density);

		particles = &region->species[n].main_vector;

		for (int i = 0; i < spec[n].main_vector.size; ++i)
		{
			t_part part = spec[n].main_vector.data[i];

			if(part.iy < region->limits_y[1] && part.iy >= region->limits_y[0])
			{
				// Check if buffer is large enough and if not reallocate
				if (particles->size + 1 > particles->size_max)
				{
					particles->size_max = particles->size_max + 1024;
					particles->data = realloc((void*) particles->data, particles->size_max * sizeof(t_part));
				}

				particles->data[particles->size++] = part;
			}
		}
	}

	//Calculate the region box
	float region_box[] = {box[0], box[1] / nx[1] * (region->limits_y[1] - region->limits_y[0])};

	// Initialise the local current
	current_new(&region->local_current, region->nx, region_box, dt);

	// Initialise the local emf
	emf_new(&region->local_emf, region->nx, region_box, dt);

	// Initialise the others regions recursively
	if (id + 1 < n_regions)
	{
		region->next = malloc(sizeof(t_region));
		assert(region->next);

		region_new(region->next, n_regions, nx, id + 1, n_spec, spec, box, dt, region);
	} else
	{
		t_region *restrict p = region;

		// Go to the first region
		while (p->id != 0) p = p->prev;

		// Bridge the last region with the first
		p->prev = region;
		region->next = p;
	}
}

// Link two adjacent regions and calculate the overlap zone between them
void region_link_adj_regions(t_region *region)
{
	current_overlap_zone(&region->local_current, &region->prev->local_current);
	emf_overlap_zone(&region->local_emf, &region->prev->local_emf);

	for(int i = 0; i < region->n_species; i++)
		spec_adjacent_vectors(&region->species[i], &region->next->species[i].temp_buffer[0],
			&region->prev->species[i].temp_buffer[1]);
}

// Set moving window
void region_set_moving_window(t_region *region)
{
	region->local_current.moving_window = true;
	region->local_emf.moving_window = true;

	for(int i = 0; i < region->n_species; i++)
		region->species[i].moving_window = true;
}

// Add a laser to all the regions
void region_add_laser(t_region *region, t_emf_laser *laser)
{
	if(region->id != 0) while(region->id != 0) region = region->next;

	t_region *p = region;
	do
	{
		emf_add_laser(&p->local_emf, laser, p->limits_y[0]);
		p = p->next;
	}while(p->id != 0);

	p = region;
	do
	{
		if(p->id != 0) emf_update_gc_y(&p->local_emf);
		p = p->next;
	}while(p->id != 0);

	p = region;
	do
	{
		div_corr_x(&p->local_emf);
		p = p->next;
	}while(p->id != 0);

	p = region;
	do
	{
		emf_update_gc_y(&p->local_emf);
		emf_update_gc_x(&p->local_emf);
		p = p->next;
	}while(p->id != 0);
}

void region_delete(t_region *region)
{
	current_delete(&region->local_current);
	emf_delete(&region->local_emf);

	for (int i = 0; i < region->n_species; i++)
		spec_delete(&region->species[i]);
	free(region->species);
}

/*********************************************************************************************
 Advance
 *********************************************************************************************/

// Spec advance and current reduction in x for all the regions (recursively)
void region_spec_advance(t_region *region)
{
	#pragma oss task in(region->local_emf.E_buf[0; region->local_emf.total_size])\
	in(region->local_emf.B_buf[0; region->local_emf.total_size]) \
	inout({region->species[n].main_vector, n = 0; region->n_species}) \
	out({region->next->species[n].temp_buffer[0], n = 0; region->n_species}) \
	out({region->prev->species[n].temp_buffer[1], n = 0; region->n_species}) \
	out(region->local_current.J_buf[0; region->local_current.total_size]) \
	label(Spec Advance)
	{
		current_zero(&region->local_current);

		for (int i = 0; i < region->n_species; i++)
			spec_advance(&region->species[i], &region->local_emf, &region->local_current, region->limits_y);

		current_reduction_x(&region->local_current);
	}

	if(region->next->id != 0) region_spec_advance(region->next);
}

// Update the particle vector in all the regions (recursively)
void region_spec_update(t_region *region)
{
	#pragma oss task in({region->species[n].temp_buffer[0:1], n = 0; region->n_species}) \
	inout({region->species[n].main_vector, n = 0; region->n_species}) label(Spec Update)
	for (int i = 0; i < region->n_species; i++)
		spec_update_main_vector(&region->species[i]);

	if(region->next->id != 0) region_spec_update(region->next);
}

// Current reduction in y for all the regions (recursive calling)
void region_current_reduction_y(t_region *region)
{
	#pragma oss task inout(region->local_current.J_buf[0;region->local_current.overlap_zone]) \
	inout(region->local_current.J_upper[-region->local_current.gc[0][0];region->local_current.overlap_zone]) \
	label(Current Reduction Y)
	current_reduction_y(&region->local_current);

	if(region->next->id != 0) region_current_reduction_y(region->next);
}

// Apply the filter to the current buffer in all regions recursively. Then is necessary to
// update the ghost cells for produce correct results
void region_current_smooth(t_region *region, enum CURRENT_SMOOTH_MODE mode)
{
	switch (mode) {
		case SMOOTH_X:
			#pragma oss task inout(region->local_current.J_buf[0;region->local_current.total_size]) \
			label(Current Smooth X)
			current_smooth_x(&region->local_current);
			break;

		case CURRENT_UPDATE_GC:
			#pragma oss task inout(region->local_current.J_buf[0; region->local_current.overlap_zone]) \
			inout(region->local_current.J_upper[-region->local_current.gc[0][0]; region->local_current.overlap_zone]) \
			label(Current Update GC)
			current_gc_update_y(&region->local_current);
			break;
		default:
			break;
	}

	if(region->next->id != 0) region_current_smooth(region->next, mode);
}

// Advance the EMF in each region recursively. Then is necessary update the ghost cells to reflect the
// new values
void region_emf_advance(t_region *region, enum EMF_UPDATE mode)
{
	switch (mode) {
		case EMF_ADVANCE:
			#pragma oss task in(region->local_current.J_buf[0; region->local_current.total_size]) \
			inout(region->local_emf.E_buf[0; region->local_emf.total_size]) \
			inout(region->local_emf.B_buf[0; region->local_emf.total_size]) \
			label(EMF Advance)
			emf_advance(&region->local_emf, &region->local_current);
			break;

		case EMF_UPDATE_GC:
			#pragma oss task inout(region->local_emf.B_buf[0; region->local_emf.overlap]) \
			inout(region->local_emf.B_upper[-region->local_emf.gc[0][0]; region->local_emf.overlap]) \
			inout(region->local_emf.E_buf[0; region->local_emf.overlap]) \
			inout(region->local_emf.E_upper[-region->local_emf.gc[0][0]; region->local_emf.overlap]) \
			label(EMF Update GC)
			emf_update_gc_y(&region->local_emf);
			break;
		default:
			break;
	}

	if(region->next->id != 0) region_emf_advance(region->next, mode);
}

// Advance one iteration for all the regions. Always begin with the first region (id = 0)
void region_advance(t_region *region)
{
	if(region->id != 0) while(region->id != 0) region = region->next;

	region_spec_advance(region);
	region_spec_update(region);
	region_current_reduction_y(region);

	if(region->local_current.smooth.xtype != NONE)
	{
		region_current_smooth(region, SMOOTH_X);
		region_current_smooth(region, CURRENT_UPDATE_GC);
	}

	region_emf_advance(region, EMF_ADVANCE);
	region_emf_advance(region, EMF_UPDATE_GC);
}

/*********************************************************************************************
 Diagnostics
 *********************************************************************************************/
void region_charge_report(const t_region *region, t_part_data *charge, int i_spec)
{
	spec_deposit_charge(&region->species[i_spec], charge);
}

void region_emf_report(const t_region *region, t_fld *restrict E_mag, t_fld *restrict B_mag, const int nrow)
{
	emf_report_magnitude(&region->local_emf, E_mag, B_mag, nrow, region->limits_y[0]);
}
