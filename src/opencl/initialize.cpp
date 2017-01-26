//------------------------------------------------------------------------------
//
//   This file is part of the VAMPIRE open source package under the
//   Free BSD licence (see licence file for details).
//
//------------------------------------------------------------------------------
//

// C++ standard library headers
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// Vampire headers
#include "atoms.hpp"
#include "cells.hpp"
#include "errors.hpp"
#include "gpu.hpp"
#include "material.hpp"
#include "stats.hpp"
#include "vio.hpp"
#include "vopencl.hpp"

// vopencl module headers
#include "data.hpp"
#include "init_kernels.hpp"
#include "internal.hpp"
#include "opencl_include.hpp"
#include "opencl_utils.hpp"
#include "statistics.hpp"
#include "typedefs.hpp"

#ifdef OPENCL
namespace vcl = ::vopencl::internal;
#endif

namespace vopencl
{
   namespace internal
   {
      cl::NDRange global;
   }

   //----------------------------------------------------------------------------
   // Function to initialize vopencl module
   //----------------------------------------------------------------------------
   bool initialize(bool cpu_stats)
   {
      bool success = false;

#ifdef OPENCL

      std::string message("OpenCL has been enabled in ");
#ifdef OPENCL_DP
      message.append("double precision mode.");
#else
      message.append("single precision mode.");
#endif // OPENCL_DP

#ifdef OPENCL_USE_NATIVE_FUNCTIONS
      message.append(" Native functions will be used.");
#endif

      std::cout << message << std::endl;
      zlog << zTs() << message << std::endl;

      //vcl::stats::use_cpu = cpu_stats;
      vcl::stats::use_cpu = true;

      // find OpenCL platforms and devices
      std::vector<cl::Platform> platforms;
      cl::Platform::get(&platforms);
      unsigned nplatforms = platforms.size();

      if (nplatforms == 0)
      {
         message = "Error: OpenCL is enabled but no platforms are available.";
         std::cout << message << std::endl;
         zlog << zTs() << message << std::endl;
         ::err::vexit();
      }

      std::vector<std::vector<cl::Device>> devices(nplatforms);
      unsigned ndevices = 0;
      for (unsigned i=0; i<nplatforms; ++i)
      {
         std::vector<cl::Device> tmp_devices;
         platforms[i].getDevices(CL_DEVICE_TYPE_ALL, &tmp_devices);
         devices[i] = tmp_devices;
         ndevices += tmp_devices.size();

#ifdef OPENCL_DEBUG
         vcl::OCLLOG << "Found platform " << platforms[i].getInfo<CL_PLATFORM_NAME>() << std::endl;
         for (unsigned j=0; j<tmp_devices.size(); ++j)
         {
            vcl::OCLLOG << "Found device " << tmp_devices[j].getInfo<CL_DEVICE_NAME>() << std::endl;
            vcl::OCLLOG << "with version " << tmp_devices[j].getInfo<CL_DEVICE_VERSION>() << std::endl;
         }
#endif // OPENCL_DEBUG
      }

      if (ndevices == 0)
      {
         message = "Error: OpenCL is enabled but no suitable devices can be found.";
         std::cout << message << std::endl;
         zlog << zTs() << message << std::endl;
         ::err::vexit();
      }

      cl::Platform default_platform = platforms[0];
      vcl::default_device = devices[0][0];

#ifdef OPENCL_DEBUG
      vcl::OCLLOG << "Using default platform " << default_platform.getInfo<CL_PLATFORM_NAME>() << std::endl;
      vcl::OCLLOG << "Using default device " << vcl::default_device.getInfo<CL_DEVICE_NAME>() << std::endl;
#endif // OPENCL_DEBUG

      vcl::context = cl::Context({vcl::default_device});

      vcl::queue = cl::CommandQueue(vcl::context, vcl::default_device, CL_QUEUE_PROFILING_ENABLE);

      if (::gpu::num_threads > 0)
      {
         vcl::global = cl::NDRange(::gpu::num_threads);
      }
      else
      {
         vcl::global = cl::NDRange(4);
      }

      success = true;

      success &= vcl::initialize_atoms();
      success &= vcl::initialize_fields();
      success &= vcl::initialize_cells();
      success &= vcl::initialize_materials();
      success &= vcl::initialize_topology();
      success &= vcl::initialize_stats();
      success &= vcl::initialize_rng();
      success &= vcl::initialize_kernels();

#endif // OPENCL

      return success;

   }

#ifdef OPENCL

   namespace internal
   {
      bool initialize_atoms(void) noexcept
      {
         // Allocate and initialize device memory for atomic spins
         vcl::atoms::spin_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue,
                                                             vcl::read_write,
                                                             ::atoms::x_spin_array,
                                                             ::atoms::y_spin_array,
                                                             ::atoms::z_spin_array);

         // Allocate and initialize device memory for atomic coordinates
         vcl::atoms::coord_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue,
                                                              vcl::read_write,
                                                              ::atoms::x_coord_array,
                                                              ::atoms::y_coord_array,
                                                              ::atoms::z_coord_array);

         const size_t real_buffer_size = ::atoms::num_atoms * sizeof(vcl::real_t);
         const size_t  int_buffer_size = ::atoms::num_atoms * sizeof(cl_int);

         // Allocate and initialize device memory for atomic information
         vcl::atoms::type_array = cl::Buffer(vcl::context, vcl::read_only, int_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::atoms::type_array, CL_FALSE, 0, int_buffer_size, &::atoms::type_array[0]);

         // Allocate and initialize cell information
         vcl::atoms::cell_array = cl::Buffer(vcl::context, vcl::read_only, int_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::atoms::cell_array, CL_FALSE, 0, int_buffer_size, &::atoms::cell_array[0]);

         // Allocate and initialize unrolled spin norm array
         vcl::atoms::spin_norm_array = cl::Buffer(vcl::context, CL_MEM_READ_WRITE, real_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::atoms::spin_norm_array, CL_FALSE, 0, real_buffer_size, &::atoms::m_spin_array[0]);

         vcl::queue.finish();

         return true;
      }

      bool initialize_fields(void) noexcept
      {
         // Allocate device memory and initialize total spin field arrays
         vcl::total_spin_field_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue, CL_MEM_READ_WRITE,
                                                                  ::atoms::x_total_spin_field_array,
                                                                  ::atoms::y_total_spin_field_array,
                                                                  ::atoms::z_total_spin_field_array);

         // Allocate device memory and initialize external field arrays
         vcl::total_external_field_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue, CL_MEM_READ_WRITE,
                                                                      ::atoms::x_total_external_field_array,
                                                                      ::atoms::y_total_external_field_array,
                                                                      ::atoms::z_total_external_field_array);

         // Allocate device memory and initialize for dipolar field
         vcl::dipolar_field_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue, CL_MEM_READ_WRITE,
                                                               ::atoms::x_dipolar_field_array,
                                                               ::atoms::y_dipolar_field_array,
                                                               ::atoms::z_dipolar_field_array);

         vcl::queue.finish();

         return true;
      }

      bool initialize_cells(void) noexcept
      {
         const size_t real_buffer_size = ::cells::num_cells * sizeof(vcl::real_t);
         const size_t  int_buffer_size = ::cells::num_cells * sizeof(cl_int);

         // Allocate device memory and initialize coordinates
         vcl::cells::coord_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue, CL_MEM_READ_WRITE,
                                                              ::cells::x_coord_array,
                                                              ::cells::y_coord_array,
                                                              ::cells::z_coord_array);

         // Allocate device memory and initialize cell magnetization
         vcl::cells::mag_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue, CL_MEM_READ_WRITE,
                                                            ::cells::x_mag_array,
                                                            ::cells::y_mag_array,
                                                            ::cells::z_mag_array);

         // Allocate device memory and initialize cell fields
         vcl::cells::field_array = vcl::Buffer3D<vcl::real_t>(vcl::context, vcl::queue, CL_MEM_READ_WRITE,
                                                              ::cells::x_field_array,
                                                              ::cells::y_field_array,
                                                              ::cells::z_field_array);

         // Allocate device memory and initialize voulme array
         vcl::cells::volume_array = cl::Buffer(vcl::context, vcl::read_only, real_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::cells::volume_array, CL_FALSE, 0, real_buffer_size, &::cells::volume_array[0]);

         // Allocate device memory and initialize number of atoms for each cell
         vcl::cells::num_atoms = cl::Buffer(vcl::context, vcl::read_only, int_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::cells::num_atoms, CL_FALSE, 0, int_buffer_size, &::cells::num_atoms_in_cell[0]);

         vcl::queue.finish();

         return true;
      }

      bool initialize_materials(void) noexcept
      {
         const size_t mat_buffer_size = ::mp::num_materials * sizeof(::mp::material[0]);

         // Allocate device memory and initialize materials array
         vcl::mp::materials = cl::Buffer(vcl::context, vcl::read_only, mat_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::mp::materials, CL_FALSE, 0, mat_buffer_size, &::mp::material[0]);


         vcl::queue.finish();

         return true;
      }

      bool initialize_topology(void) noexcept
      {
         const size_t limits_buffer_size = (::atoms::num_atoms+1) * sizeof(::atoms::num_atoms);
         const size_t neighbours_buffer_size = ::atoms::neighbour_list_array.size() * sizeof(::atoms::neighbour_list_array[0]);

         std::vector<cl_int> limits_h(::atoms::num_atoms+1, 0);
         for (int atom=0; atom<::atoms::num_atoms; ++atom)
            limits_h[atom+1] = ::atoms::neighbour_list_end_index[atom]+1;

         // Allocate device memory and initialize limits array
         vcl::atoms::limits = cl::Buffer(vcl::context, vcl::read_only, limits_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::atoms::limits, CL_FALSE, 0, limits_buffer_size, &limits_h[0]);

         vcl::atoms::neighbours = cl::Buffer(vcl::context, vcl::read_only, neighbours_buffer_size);
         vcl::queue.enqueueWriteBuffer(vcl::atoms::neighbours, CL_FALSE, 0, neighbours_buffer_size, &::atoms::neighbour_list_array[0]);

         vcl::queue.finish();

         return true;
      }

      bool initialize_stats(void) noexcept
      {
         std::vector<cl_int> mask;
         std::vector<double> saturations;

         // system magnetization
         ::stats::system_magnetization.get_mask(mask, saturations);
         vcl::stats::system_mask_size = saturations.size();
         const size_t sys_mask_buffer_size = mask.size() * sizeof(mask[0]);
         const size_t sys_sats_buffer_size = 4 * saturations.size() * sizeof(vcl::real_t);
         if (sys_mask_buffer_size != 0)
         {
            vcl::stats::system_mask = cl::Buffer(vcl::context, vcl::read_write, sys_mask_buffer_size);
            vcl::queue.enqueueWriteBuffer(vcl::stats::system_mask, CL_FALSE, 0, sys_mask_buffer_size, &mask[0]);
         }
         if (sys_sats_buffer_size != 0)
         {
            vcl::stats::system_magnetization = cl::Buffer(vcl::context, vcl::read_write, sys_sats_buffer_size);
            vcl::stats::system_mean_magnetization = cl::Buffer(vcl::context, vcl::read_write, sys_sats_buffer_size);
         }

         // material magnetization
         ::stats::material_magnetization.get_mask(mask, saturations);
         vcl::stats::material_mask_size = saturations.size();
         const size_t mat_mask_buffer_size = mask.size() * sizeof(mask[0]);
         const size_t mat_sats_buffer_size = 4 * saturations.size() * sizeof(vcl::real_t);
         if (mat_mask_buffer_size != 0)
         {
            vcl::stats::material_mask = cl::Buffer(vcl::context, vcl::read_write, mat_mask_buffer_size);
            vcl::queue.enqueueWriteBuffer(vcl::stats::material_mask, CL_FALSE, 0, mat_mask_buffer_size, &mask[0]);
         }
         if (mat_sats_buffer_size != 0)
         {
            vcl::stats::material_magnetization = cl::Buffer(vcl::context, vcl::read_write, mat_sats_buffer_size);
            vcl::stats::material_mean_magnetization = cl::Buffer(vcl::context, vcl::read_write, mat_sats_buffer_size);
         }

         // height magnetization
         ::stats::height_magnetization.get_mask(mask, saturations);
         vcl::stats::height_mask_size = saturations.size();
         const size_t height_mask_buffer_size = mask.size() * sizeof(mask[0]);
         const size_t height_sats_buffer_size = 4 * saturations.size() * sizeof(vcl::real_t);
         if (height_mask_buffer_size != 0)
         {
            vcl::stats::height_mask = cl::Buffer(vcl::context, vcl::read_write, height_mask_buffer_size);
            vcl::queue.enqueueWriteBuffer(vcl::stats::height_mask, CL_FALSE, 0, height_mask_buffer_size, &mask[0]);
         }
         if (height_sats_buffer_size != 0)
         {
            vcl::stats::height_magnetization = cl::Buffer(vcl::context, vcl::read_write, height_sats_buffer_size);
            vcl::stats::height_mean_magnetization = cl::Buffer(vcl::context, vcl::read_write, height_sats_buffer_size);
         }

         // material height magnetization
         ::stats::material_height_magnetization.get_mask(mask, saturations);
         vcl::stats::material_height_mask_size = saturations.size();
         const size_t mat_h_mask_buffer_size = mask.size() * sizeof(mask[0]);
         const size_t mat_h_sats_buffer_size = 4 * saturations.size() * sizeof(vcl::real_t);
         if (mat_h_mask_buffer_size != 0)
         {
            vcl::stats::material_height_mask = cl::Buffer(vcl::context, vcl::read_write, mat_h_mask_buffer_size);
            vcl::queue.enqueueWriteBuffer(vcl::stats::material_height_mask, CL_FALSE, 0, mat_h_mask_buffer_size, &mask[0]);
         }
         if (mat_h_sats_buffer_size != 0)
         {
            vcl::stats::material_height_magnetization = cl::Buffer(vcl::context, vcl::read_write, mat_h_sats_buffer_size);
            vcl::stats::material_height_mean_magnetization = cl::Buffer(vcl::context, vcl::read_write, mat_h_sats_buffer_size);
         }

         return true;
      }

      static cl_ulong rand64(void) noexcept
      {
         cl_ulong r = std::rand();
         return r << 32 | std::rand();
      }

      bool initialize_rng(void) noexcept
      {
         // each atom needs three random numbers per Heun step
         std::vector<cl_ulong> rs(::atoms::num_atoms*3);

         const size_t u_buffer_size = rs.size() * sizeof(cl_ulong);
         const size_t g_buffer_size = rs.size() * sizeof(vcl::real_t);

         vcl::rng::urands = cl::Buffer(vcl::context, vcl::read_write, u_buffer_size);
         vcl::rng::grands = cl::Buffer(vcl::context, vcl::read_write, g_buffer_size);

         std::srand(1);  // constant for now to get deterministic results
         for (auto &elem : rs)
            // must not seed xorshift with 0
            do { elem = rand64(); } while (elem == 0);

         vcl::queue.enqueueWriteBuffer(vcl::rng::urands, CL_TRUE, 0, u_buffer_size, &rs[0]);

         vcl::queue.finish();

         return true;
      }
   }

#endif // OPENCL

} // end of vopencl namespace

