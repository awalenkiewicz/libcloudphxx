// vim:filetype=cpp
/** @file
  * @copyright University of Warsaw
  * @section LICENSE
  * GPLv3+ (see the COPYING file or http://www.gnu.org/licenses/)
  */
#include <thrust/sequence.h>

namespace libcloudphxx
{
  namespace lgrngn
  {
    namespace detail
    {
      struct arbitrary_sequence //fill container with n 0s, m 1s, l 2s, etc...
      {
        thrust_device::pointer<thrust_size_t> res;
        arbitrary_sequence(thrust_device::pointer<thrust_size_t> res): res(res) {}
      
        template<typename Tuple>
        BOOST_GPU_ENABLED
        void operator()(Tuple tup)
        {
          for(int i=0; i<thrust::get<0>(tup); ++i)
            *(res+i+thrust::get<1>(tup)) = thrust::get<2>(tup);
        }
      };
      
      template<typename real_t>
      struct pos_lgrngn_domain
      // get a random position within ii-th cell taking into account Lagrangian domain
      {
        real_t p0, // lower bound of the Lagrangian domain
               p1, // upper bound of the Lagrangian domain
               dp; // cell size of the Eulerian grid

        pos_lgrngn_domain(real_t p0, real_t p1, real_t dp): p0(p0), p1(p1), dp(dp) {}

        BOOST_GPU_ENABLED
        real_t operator()(real_t u01, thrust_size_t ii) // random number [0,1), cell index in respective dimension
        {
#if !defined(__NVCC__)
          using std::min;
          using std::max;
#endif
        	
          return u01 * min(p1, (ii+1) * dp) + (1. - u01) * max(p0, ii * dp); 
        }
      };
    };

    // init i,j,k,x,y,z based on the number of SDs to init in each cell stored in count_num
    // reused in source
    template <typename real_t, backend_t device>
    void particles_t<real_t, device>::impl::init_xyz_helper()
    {
      n_part_old = n_part; // initially 0
      n_part_to_init = thrust::reduce(count_num.begin(), count_num.end());
      if(n_part_to_init == 0) return;
      n_part += n_part_to_init;
      hskpng_resize_npart();      

      thrust_device::vector<thrust_size_t> &ptr(tmp_device_size_cell);
      thrust::exclusive_scan(count_num.begin(), count_num.end(), ptr.begin()); // number of SDs in cells to init up to (i-1)

      // fill ijk with cell number of each SD
      thrust::for_each(
        thrust::make_zip_iterator(thrust::make_tuple(
          count_num.begin(), ptr.begin(), thrust::make_counting_iterator(0)
        )), 
        thrust::make_zip_iterator(thrust::make_tuple(
          count_num.end(), ptr.end(), thrust::make_counting_iterator(n_cell)
        )), 
        detail::arbitrary_sequence(&(ijk[n_part_old]))
      );

      // get i, j, k from ijk 
      switch(n_dims)
      {
        case 3:
          namespace arg = thrust::placeholders;
          // y
          thrust::transform(
            ijk.begin() + n_part_old, ijk.end(), // input - first arg
            j.begin() + n_part_old,        // output
            (arg::_1 / opts_init.nz) % (opts_init.ny) // z varies first
          );
          // z
          thrust::transform(
            ijk.begin() + n_part_old, ijk.end(), // input - first arg
            k.begin() + n_part_old,        // output
            arg::_1 % (opts_init.nz)   // z varies first
          );
          // x
          thrust::transform(
            ijk.begin() + n_part_old, ijk.end(), // input - first arg
            i.begin() + n_part_old,        // output
            arg::_1 / (opts_init.nz * opts_init.ny)    // z and y vary first
          );
          break;
        case 2:
          // z
          thrust::transform(
            ijk.begin() + n_part_old, ijk.end(), // input - first arg
            k.begin() + n_part_old,        // output
            arg::_1 % (opts_init.nz)   // z varies first
          );
          // x
          thrust::transform(
            ijk.begin() + n_part_old, ijk.end(), // input - first arg
            i.begin() + n_part_old,        // output
            arg::_1 / (opts_init.nz)
          );
          break;
        case 1:
          thrust::copy(ijk.begin() + n_part_old, ijk.end(), i.begin() + n_part_old); // only x
        case 0:
          break;
        default:
          assert(false);
          break;
      }

      thrust_device::vector<real_t> 
                  *v[3] = { &x,           &y,           &z           };
      const int    n[3] = { opts_init.nx, opts_init.ny, opts_init.nz };
      const real_t a[3] = { opts_init.x0, opts_init.y0, opts_init.z0 };
      const real_t b[3] = { opts_init.x1, opts_init.y1, opts_init.z1 };
      const real_t d[3] = { opts_init.dx, opts_init.dy, opts_init.dz };
      thrust_device::vector<thrust_size_t> 
                  *ii[3] = { &i,           &j,           &k           };

      for (int ix = 0; ix < 3; ++ix)
      {
        if (n[ix] == 0) continue;

        // tossing random numbers [0,1] 
        rand_u01(n_part_to_init);

	// shifting from [0,1] to random position within respective cell 
        {
          namespace arg = thrust::placeholders;
	  thrust::transform(
	    u01.begin(), 
	    u01.begin() + n_part_to_init,
            ii[ix]->begin() + n_part_old, 
	    v[ix]->begin() + n_part_old, 
            detail::pos_lgrngn_domain<real_t>(a[ix], b[ix], d[ix])
	  );
        }
      }
    }

    // Init SD positions. Particles are considered to be sorted by cell number, in order
    // to obtain uniform initial distribution in each cell (see particles_impl_init_dry)
    template <typename real_t, backend_t device>
    void particles_t<real_t, device>::impl::init_xyz()
    {

      if(n_dims > 0)
      {
        namespace arg = thrust::placeholders;
        // some cells may be used only partially in thr super-droplet method
        // e.g. when Lagrangian domain (x0, x1, etc...) is smaller than the 
        // Eulerian domain (0, nx*dx, etc...)
        // sd_conc defines number of SDs per Eulerian cell
        thrust::transform(dv.begin(), dv.end(), count_num.begin(), (real_t(opts_init.sd_conc) * arg::_1 / (opts_init.dx * opts_init.dy * opts_init.dz) + real_t(0.5))); 
      }
      // parcel setup
      else
        thrust::fill(count_num.begin(), count_num.end(), opts_init.sd_conc);

      init_xyz_helper(); 
    }
  };
};
