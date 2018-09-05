// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "../../config/config.hpp"
#if defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_OCCA)

#include "backend.hpp"
#include "url_handler.hpp"
#include "bilinearform.hpp"
#include "../../general/array.hpp"
#include "../hypre/layout.hpp"

namespace mfem
{

namespace occa
{

bool Engine::fileOpenerRegistered = false;

void Engine::Init(const std::string &engine_spec)
{
   //
   // Initialize inherited fields
   //
   memory_resources[0] = NULL;
   workers_weights[0]= 1.0;
   workers_mem_res[0] = 0;

   //
   // Initialize the OCCA engine
   //
   ::occa::properties props(engine_spec);
   device = new ::occa::device[1];
   device[0].setup(props);
   std::string integ_type(props.get("integrator", std::string("occa")));
   use_acrotensor = !strcmp(integ_type.c_str(), "acrotensor");

   okl_path = "mfem-occa://";
   if (!fileOpenerRegistered)
   {
      // The directories from "MFEM_OCCA_OKL_PATH", if any, have the highest
      // priority.
      FileOpener *fo = new FileOpener("mfem-occa://", "MFEM_OCCA_OKL_PATH");
      // Next in priority is the source path, if it exists.
      std::string mfem_src_prefix = mfem::GetSourcePath();
      fo->AddDir(mfem_src_prefix + "/backends/occa");
      // And last in priority is the install path, if it exists.
      std::string mfem_install_prefix = mfem::GetInstallPath();
      fo->AddDir(mfem_install_prefix + "/lib/mfem/occa");
      ::occa::io::fileOpener::add(fo);
      fileOpenerRegistered = true;
   }
   // std::cout << "OCCA device properties:\n" << device[0].properties();

   force_cuda_aware_mpi = false;
}

Engine::Engine(const std::string &engine_spec)
   : mfem::Engine(NULL, 1, 1)
{
   Init(engine_spec);
}

#ifdef MFEM_USE_MPI
Engine::Engine(MPI_Comm _comm, const std::string &engine_spec)
   : mfem::Engine(NULL, 1, 1)
{
   comm = _comm;
   Init(engine_spec);
}
#endif

bool Engine::CheckEngine(const mfem::Engine *engine) const
{
   return (engine != NULL && util::Is<const Engine>(engine) != NULL &&
           *util::As<const Engine>(engine) == *this);
}

bool Engine::CheckLayout(const PLayout *layout) const
{
   return (layout != NULL && util::Is<const Layout>(layout) != NULL &&
           layout->As<Layout>().OccaEngine() == *this);
}

bool Engine::CheckArray(const PArray *array) const
{
   return (array != NULL && util::Is<const Array>(array) != NULL &&
           array->As<Array>().OccaEngine() == *this);
}

bool Engine::CheckVector(const PVector *vector) const
{
   return (vector != NULL && util::Is<const Vector>(vector) != NULL &&
           vector->As<Vector>().OccaEngine() == *this);
}

bool Engine::CheckFESpace(const PFiniteElementSpace *fes) const
{
   return (fes != NULL && util::Is<const FiniteElementSpace>(fes) != NULL &&
           fes->As<FiniteElementSpace>().OccaEngine() == *this);
}

DLayout Engine::MakeLayout(std::size_t size) const
{
   return DLayout(new Layout(*this, size));
}

DLayout Engine::MakeLayout(const mfem::Array<std::size_t> &offsets) const
{
   MFEM_ASSERT(offsets.Size() == 2,
               "multiple workers are not supported yet");
   return DLayout(new Layout(*this, offsets.Last()));
}

DArray Engine::MakeArray(PLayout &layout, std::size_t item_size) const
{
   return DArray(new Array(layout.As<Layout>(), item_size));
}

DVector Engine::MakeVector(PLayout &layout, int type_id) const
{
   MFEM_ASSERT(type_id == ScalarId<double>::value, "invalid type_id");
   // MFEM_ASSERT(dynamic_cast<Layout *>(&layout) != NULL,
   //             "invalid input layout");
   if (dynamic_cast<Layout *>(&layout))
   {
      Layout *lt = static_cast<Layout *>(&layout);
      return DVector(new Vector(*lt));
   }
   else if (dynamic_cast<mfem::hypre::Layout *>(&layout))
   {
      // FIXME There are two ways of doing this:
      // 1. Have operators carry around hypre layouts and be able to initialize vectors directly from them.
      // 2. convert all layouts to the occa layouts
      // Currently using the first option
      mfem::hypre::Layout &lt = static_cast<mfem::hypre::Layout&>(layout);
      Layout &occa_lt = static_cast<Layout&>(lt.Base());
      return DVector(new Vector(occa_lt));
      // mfem::hypre::Layout *lt = static_cast<mfem::hypre::Layout *>(&layout);
      // // TODO Clean this up with a trait class...
      // return DVector(new mfem::hypre::Vector<
      //                mfem::occa::Array,
      //                mfem::occa::Vector,
      //                mfem::occa::Engine,
      //                mfem::occa::Layout>(*lt));
   }
   else
   {
      mfem_error("ERROR: [mfem::occa::Engine::MakeVector] cannot interpret layout");
   }
}

DFiniteElementSpace Engine::MakeFESpace(mfem::FiniteElementSpace &fespace) const
{
   return DFiniteElementSpace(new FiniteElementSpace(*this, fespace));
}

DBilinearForm Engine::MakeBilinearForm(mfem::BilinearForm &bf) const
{
   return DBilinearForm(new BilinearForm(*this, bf));
}

void Engine::AssembleLinearForm(LinearForm &l_form) const
{
   /// FIXME - What will the actual parameters be?
   MFEM_ABORT("FIXME");
}

mfem::Operator *Engine::MakeOperator(const MixedBilinearForm &mbl_form) const
{
   /// FIXME - What will the actual parameters be?
   MFEM_ABORT("FIXME");
   return NULL;
}

mfem::Operator *Engine::MakeOperator(const NonlinearForm &nl_form) const
{
   /// FIXME - What will the actual parameters be?
   MFEM_ABORT("FIXME");
   return NULL;
}

} // namespace mfem::occa

} // namespace mfem

#endif // defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_OCCA)
