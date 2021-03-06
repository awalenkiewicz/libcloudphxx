// vim:filetype=cpp
/** @file
  * @copyright University of Warsaw
  * @section LICENSE
  * GPLv3+ (see the COPYING file or http://www.gnu.org/licenses/)
  * @brief initialisation routine for super droplets
  */

namespace libcloudphxx
{
  namespace lgrngn
  {
    // init
    template <typename real_t, backend_t device>
    void particles_t<real_t, device>::init(
      const arrinfo_t<real_t> th,
      const arrinfo_t<real_t> rv,
      const arrinfo_t<real_t> rhod,
      const arrinfo_t<real_t> courant_x, // might be NULL
      const arrinfo_t<real_t> courant_y, // might be NULL
      const arrinfo_t<real_t> courant_z, // might be NULL
      const std::map<enum chem_species_t, const arrinfo_t<real_t> > ambient_chem
    )
    {
      if (pimpl->init_called) 
        throw std::runtime_error("init() may be called just once");
      pimpl->init_called = true;

      // sanity checks
      if (th.is_null() || rv.is_null() || rhod.is_null())
        throw std::runtime_error("passing th, rv and rhod is mandatory");

      // --------  init cell characteristics  --------
      // initialising Eulerian-Lagrandian coupling
      if (!courant_x.is_null() || !courant_y.is_null() || !courant_z.is_null())
      {
	if (pimpl->n_dims == 0)
	  throw std::runtime_error("Courant numbers passed in 0D setup");

	if (pimpl->n_dims == 1 && (courant_x.is_null() || !courant_y.is_null() || !courant_z.is_null()))
	  throw std::runtime_error("Only X Courant number allowed in 1D setup");

	if (pimpl->n_dims == 2 && (courant_x.is_null() || !courant_y.is_null() || courant_z.is_null()))
	  throw std::runtime_error("Only X and Z Courant numbers allowed in 2D setup");

	if (pimpl->n_dims == 3 && (courant_x.is_null() || courant_y.is_null() || courant_z.is_null()))
	  throw std::runtime_error("All XYZ Courant number components required in 3D setup");
      }

      if (pimpl->opts_init.chem_switch && ambient_chem.size() != chem_gas_n) 
        throw std::runtime_error("chemistry was not switched off and ambient_chem is empty");

      if (!pimpl->opts_init.chem_switch && ambient_chem.size() != 0) 
        throw std::runtime_error("chemistry was switched off and ambient_chem is not empty");

      // TODO: in source match chemical composition
      if (pimpl->opts_init.chem_switch && pimpl->opts_init.src_switch) 
        throw std::runtime_error("chemistry and aerosol source are not compatible");

      if(pimpl->opts_init.dry_distros.size() > 1 && pimpl->opts_init.chem_switch)
        throw std::runtime_error("chemistry and multiple kappa distributions are not compatible");

      // TODO: in source match kappas 
      if(pimpl->opts_init.dry_distros.size() > 1 && pimpl->opts_init.src_switch)
        throw std::runtime_error("aerosol source and multiple kappa distributions are not compatible");


      // initialising Eulerian-Lagrangian coupling
      pimpl->init_sync();  // also, init of ambient_chem vectors
      pimpl->init_e2l(th,   &pimpl->th);
      pimpl->init_e2l(rv,   &pimpl->rv);
      pimpl->init_e2l(rhod, &pimpl->rhod);

#if !defined(__NVCC__)
      using std::max;
#endif
      if (!courant_x.is_null()) pimpl->init_e2l(courant_x, &pimpl->courant_x, 1, 0, 0);
      if (!courant_y.is_null()) pimpl->init_e2l(courant_y, &pimpl->courant_y, 0, 1, 0, pimpl->n_x_bfr * pimpl->opts_init.nz);
      if (!courant_z.is_null()) pimpl->init_e2l(courant_z, &pimpl->courant_z, 0, 0, 1, pimpl->n_x_bfr * max(1, pimpl->opts_init.ny));

      if (pimpl->opts_init.chem_switch)
	for (int i = 0; i < chem_gas_n; ++i)
	  pimpl->init_e2l(ambient_chem.at((chem_species_t)i), &pimpl->ambient_chem[(chem_species_t)i]);

      // feeding in Eulerian fields
      pimpl->sync(th,   pimpl->th);
      pimpl->sync(rv,   pimpl->rv);
      pimpl->sync(rhod, pimpl->rhod);

      if (!courant_x.is_null()) pimpl->sync(courant_x, pimpl->courant_x);
      if (!courant_y.is_null()) pimpl->sync(courant_y, pimpl->courant_y);
      if (!courant_z.is_null()) pimpl->sync(courant_z, pimpl->courant_z);

      if (pimpl->opts_init.chem_switch)
	for (int i = 0; i < chem_gas_n; ++i)
	  pimpl->sync(
            ambient_chem.at((chem_species_t)i), 
            pimpl->ambient_chem[(chem_species_t)i]
          );

      // initialising housekeeping data of the size ncell
      pimpl->init_hskpng_ncell(); 

      // initialising helper data for advection (Arakawa-C grid neighbours' indices)
      // done before init_xyz, cause it uses dv initialized here
      pimpl->init_grid();

      // initialising Tpr
      pimpl->hskpng_Tpr(); 

      pimpl->init_sstp();

      // --------  init super-droplet characteristics  --------
      // reserve memory for data of the size of the max number of SDs
      pimpl->init_hskpng_npart(); 

      // calc sum of ln(rd) ranges of all distributions
      real_t tot_lnrd_rng = 0.;
      for (typename opts_init_t<real_t>::dry_distros_t::const_iterator ddi = pimpl->opts_init.dry_distros.begin(); ddi != pimpl->opts_init.dry_distros.end(); ++ddi)
      {
        pimpl->dist_analysis(
          ddi->second,
          pimpl->opts_init.sd_conc
        );
        tot_lnrd_rng += pimpl->log_rd_max - pimpl->log_rd_min;
      }

      pimpl->n_part_old = 0;

      // initialize SDs of each kappa-type
      for (typename opts_init_t<real_t>::dry_distros_t::const_iterator ddi = pimpl->opts_init.dry_distros.begin(); ddi != pimpl->opts_init.dry_distros.end(); ++ddi)
      {
        // analyze the distribution, TODO: just did it
        pimpl->dist_analysis(
          ddi->second,
          pimpl->opts_init.sd_conc
        );

        real_t fraction = (pimpl->log_rd_max - pimpl->log_rd_min) / tot_lnrd_rng;
        // adjust the multiplicity init coefficient to smaller number of SDs representing this kappa-type
        pimpl->multiplier *= pimpl->opts_init.sd_conc / int(fraction * pimpl->opts_init.sd_conc + 0.5);

        // init number of SDs of this kappa in cells, TODO: due to rounding, we might end up with not exactly sd_conc SDs per cell...
        pimpl->init_count_num(fraction);
  
        // update no of particles
        // TODO: move to a separate function
        pimpl->n_part_old = pimpl->n_part;
        pimpl->n_part_to_init = thrust::reduce(pimpl->count_num.begin(), pimpl->count_num.end());
        pimpl->n_part += pimpl->n_part_to_init;
        pimpl->hskpng_resize_npart(); 
  
        // init ijk vector, also n_part and resize n_part vectors
        pimpl->init_ijk();
  
        // initialising dry radii (needs ijk and rhod)
        pimpl->init_dry();
  
        // init multiplicities
        pimpl->init_n(
          ddi->first,
          ddi->second
        ); // TODO: document that n_of_lnrd_stp is expected!
  
        // initialising wet radii
        pimpl->init_wet();

        // memory allocation for chemical reactions, done after init.grid to have npart defined
        if(pimpl->opts_init.chem_switch){
          pimpl->init_chem();
        }

        // initialising mass of chemical compounds in droplets (needs to be done after dry radius)
        if(pimpl->opts_init.chem_switch){
          pimpl->init_chem_aq();
        }
       
        // init for substepping for chem reactions
        if(pimpl->opts_init.chem_switch){
         pimpl->init_sstp_chem();
        }

        // calculate initail volume (helper for Henry in chem)
        if (pimpl->opts_init.chem_switch){
          pimpl->chem_vol_ante();
        }
  
        // initialising particle positions
        pimpl->init_xyz();
      }
      // --------  other inits  --------
      //initialising collision kernel
      if(pimpl->opts_init.coal_switch) pimpl->init_kernel();

      //initialising vterm
      if(pimpl->opts_init.coal_switch || pimpl->opts_init.sedi_switch) pimpl->init_vterm();

      // init count_num and count_ijk
      pimpl->hskpng_count();
    }
  };
};
