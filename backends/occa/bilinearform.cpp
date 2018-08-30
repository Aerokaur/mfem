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
#include "bilininteg.hpp"
#include "abilininteg.hpp"
#include "../../fem/bilinearform.hpp"
#include "../hypre/fespace.hpp"
#include "../hypre/parmatrix.hpp"

namespace mfem
{

namespace occa
{

OccaBilinearForm::OccaBilinearForm(FiniteElementSpace *ofespace_) :
   Operator(ofespace_->OccaVLayout()),
   localX(ofespace_->OccaEVLayout()),
   localY(ofespace_->OccaEVLayout())
{
   Init(ofespace_->OccaEngine(), ofespace_, ofespace_);
}

OccaBilinearForm::OccaBilinearForm(FiniteElementSpace *otrialFESpace_,
                                   FiniteElementSpace *otestFESpace_) :
   Operator(otrialFESpace_->OccaVLayout(),
            otestFESpace_->OccaVLayout()),
   localX(otrialFESpace_->OccaEVLayout()),
   localY(otestFESpace_->OccaEVLayout())
{
   Init(otrialFESpace_->OccaEngine(), otrialFESpace_, otestFESpace_);
}

void OccaBilinearForm::Init(const Engine &e,
                            FiniteElementSpace *otrialFESpace_,
                            FiniteElementSpace *otestFESpace_)
{
   engine.Reset(&e);

   otrialFESpace = otrialFESpace_;
   trialFESpace  = otrialFESpace_->GetFESpace();

   otestFESpace = otestFESpace_;
   testFESpace  = otestFESpace_->GetFESpace();

   mesh = trialFESpace->GetMesh();

   const int elements = GetNE();

   const int trialVDim = trialFESpace->GetVDim();

   const int trialLocalDofs = otrialFESpace->GetLocalDofs();
   const int testLocalDofs  = otestFESpace->GetLocalDofs();

   // First-touch policy when running with OpenMP
   if (GetDevice().mode() == "OpenMP")
   {
      const std::string &okl_path = OccaEngine().GetOklPath();
      ::occa::kernel initLocalKernel =
         GetDevice().buildKernel(okl_path + "utils.okl",
                                 "InitLocalVector");

      const std::size_t sd = sizeof(double);
      const uint64_t trialEntries = sd * (elements * trialLocalDofs);
      const uint64_t testEntries  = sd * (elements * testLocalDofs);
      for (int v = 0; v < trialVDim; ++v)
      {
         const uint64_t trialOffset = v * trialEntries;
         const uint64_t testOffset  = v * testEntries;

         initLocalKernel(elements, trialLocalDofs,
                         localX.OccaMem().slice(trialOffset, trialEntries));
         initLocalKernel(elements, testLocalDofs,
                         localY.OccaMem().slice(testOffset, testEntries));
      }
   }
}

int OccaBilinearForm::BaseGeom() const
{
   return mesh->GetElementBaseGeometry();
}

int OccaBilinearForm::GetDim() const
{
   return mesh->Dimension();
}

int64_t OccaBilinearForm::GetNE() const
{
   return mesh->GetNE();
}

Mesh& OccaBilinearForm::GetMesh() const
{
   return *mesh;
}

FiniteElementSpace& OccaBilinearForm::GetTrialOccaFESpace() const
{
   return *otrialFESpace;
}

FiniteElementSpace& OccaBilinearForm::GetTestOccaFESpace() const
{
   return *otestFESpace;
}

mfem::FiniteElementSpace& OccaBilinearForm::GetTrialFESpace() const
{
   return *trialFESpace;
}

mfem::FiniteElementSpace& OccaBilinearForm::GetTestFESpace() const
{
   return *testFESpace;
}

int64_t OccaBilinearForm::GetTrialNDofs() const
{
   return trialFESpace->GetNDofs();
}

int64_t OccaBilinearForm::GetTestNDofs() const
{
   return testFESpace->GetNDofs();
}

int64_t OccaBilinearForm::GetTrialVDim() const
{
   return trialFESpace->GetVDim();
}

int64_t OccaBilinearForm::GetTestVDim() const
{
   return testFESpace->GetVDim();
}

const FiniteElement& OccaBilinearForm::GetTrialFE(const int i) const
{
   return *(trialFESpace->GetFE(i));
}

const FiniteElement& OccaBilinearForm::GetTestFE(const int i) const
{
   return *(testFESpace->GetFE(i));
}

// Adds new Domain Integrator.
void OccaBilinearForm::AddDomainIntegrator(OccaIntegrator *integrator,
                                           const ::occa::properties &props)
{
   AddIntegrator(integrator, props, DomainIntegrator);
}

// Adds new Boundary Integrator.
void OccaBilinearForm::AddBoundaryIntegrator(OccaIntegrator *integrator,
                                             const ::occa::properties &props)
{
   AddIntegrator(integrator, props, BoundaryIntegrator);
}

// Adds new interior Face Integrator.
void OccaBilinearForm::AddInteriorFaceIntegrator(OccaIntegrator *integrator,
                                                 const ::occa::properties &props)
{
   AddIntegrator(integrator, props, InteriorFaceIntegrator);
}

// Adds new boundary Face Integrator.
void OccaBilinearForm::AddBoundaryFaceIntegrator(OccaIntegrator *integrator,
                                                 const ::occa::properties &props)
{
   AddIntegrator(integrator, props, BoundaryFaceIntegrator);
}

// Adds Integrator based on OccaIntegratorType
void OccaBilinearForm::AddIntegrator(OccaIntegrator *integrator,
                                     const ::occa::properties &props,
                                     const OccaIntegratorType itype)
{
   if (integrator == NULL)
   {
      std::stringstream error_ss;
      error_ss << "OccaBilinearForm::";
      switch (itype)
      {
         case DomainIntegrator      : error_ss << "AddDomainIntegrator";       break;
         case BoundaryIntegrator    : error_ss << "AddBoundaryIntegrator";     break;
         case InteriorFaceIntegrator: error_ss << "AddInteriorFaceIntegrator"; break;
         case BoundaryFaceIntegrator: error_ss << "AddBoundaryFaceIntegrator"; break;
      }
      error_ss << " (...):\n"
               << "  Integrator is NULL";
      const std::string error = error_ss.str();
      mfem_error(error.c_str());
   }
   integrator->SetupIntegrator(*this, baseKernelProps + props, itype);
   integrators.push_back(integrator);
}

const mfem::Operator* OccaBilinearForm::GetTrialProlongation() const
{
   return otrialFESpace->GetProlongationOperator();
}

const mfem::Operator* OccaBilinearForm::GetTestProlongation() const
{
   return otestFESpace->GetProlongationOperator();
}

const mfem::Operator* OccaBilinearForm::GetTrialRestriction() const
{
   return otrialFESpace->GetRestrictionOperator();
}

const mfem::Operator* OccaBilinearForm::GetTestRestriction() const
{
   return otestFESpace->GetRestrictionOperator();
}

void OccaBilinearForm::Assemble()
{
   // [MISSING] Find geometric information that is needed by intergrators
   //             to share between integrators.
   const int integratorCount = (int) integrators.size();
   for (int i = 0; i < integratorCount; ++i)
   {
      integrators[i]->Assemble();
   }
}

void OccaBilinearForm::AssembleElementMatrices(DenseTensor &element_matrices)
{
   if (integrators.size() > 1) mfem_error("TBD");
   mfem::acro::PAIntegrator *pai = dynamic_cast<mfem::acro::PAIntegrator*>(integrators[0]);
   if (pai) pai->BatchedAssembleElementMatrices(element_matrices);
}

void OccaBilinearForm::FormLinearSystem(const mfem::Array<int> &constraintList,
                                        mfem::Vector &x, mfem::Vector &b,
                                        mfem::Operator *&Aout,
                                        mfem::Vector &X, mfem::Vector &B,
                                        int copy_interior)
{
   FormOperator(constraintList, Aout);
   InitRHS(constraintList, x, b, Aout, X, B, copy_interior);
}

void OccaBilinearForm::FormOperator(const mfem::Array<int> &constraintList,
                                    mfem::Operator *&Aout)
{
   const mfem::Operator *trialP = GetTrialProlongation();
   const mfem::Operator *testP  = GetTestProlongation();
   mfem::Operator *rap = this;

   if (trialP)
   {
      rap = new RAPOperator(*testP, *this, *trialP);
   }

   Aout = new OccaConstrainedOperator(rap, constraintList,
                                      rap != this);
}

void OccaBilinearForm::InitRHS(const mfem::Array<int> &constraintList,
                               mfem::Vector &x, mfem::Vector &b,
                               mfem::Operator *A,
                               mfem::Vector &X, mfem::Vector &B,
                               int copy_interior)
{
   // FIXME: move these kernels to the Backend?
   static ::occa::kernelBuilder get_subvector_builder =
      ::occa::linalg::customLinearMethod(
         "vector_get_subvector",

         "const int dof_i = v2[i];"
         "v0[i] = dof_i >= 0 ? v1[dof_i] : -v1[-dof_i - 1];",

         "defines: {"
         "  VTYPE0: 'double',"
         "  VTYPE1: 'double',"
         "  VTYPE2: 'int',"
         "  TILESIZE: 128,"
         "}");

   static ::occa::kernelBuilder set_subvector_builder =
      ::occa::linalg::customLinearMethod(
         "vector_set_subvector",
         "const int dof_i = v2[i];"
         "if (dof_i >= 0) { v0[dof_i]      = v1[i]; }"
         "else            { v0[-dof_i - 1] = -v1[i]; }",

         "defines: {"
         "  VTYPE0: 'double',"
         "  VTYPE1: 'double',"
         "  VTYPE2: 'int',"
         "  TILESIZE: 128,"
         "}");

   const mfem::Operator *P = GetTrialProlongation();
   const mfem::Operator *R = GetTrialRestriction();

   if (P)
   {
      // Variational restriction with P
      B.Resize(P->InLayout());
      P->MultTranspose(b, B);
      X.Resize(R->OutLayout());
      R->Mult(x, X);
   }
   else
   {
      // rap, X and B point to the same data as this, x and b
      X.MakeRef(x);
      B.MakeRef(b);
   }

   if (!copy_interior && constraintList.Size() > 0)
   {
      ::occa::kernel get_subvector_kernel =
         get_subvector_builder.build(GetDevice());
      ::occa::kernel set_subvector_kernel =
         set_subvector_builder.build(GetDevice());

      const Array &constrList = constraintList.Get_PArray()->As<Array>();
      Vector subvec(constrList.OccaLayout());

      get_subvector_kernel(constraintList.Size(),
                           subvec.OccaMem(),
                           X.Get_PVector()->As<Vector>().OccaMem(),
                           constrList.OccaMem());

      X.Fill(0.0);

      set_subvector_kernel(constraintList.Size(),
                           X.Get_PVector()->As<Vector>().OccaMem(),
                           subvec.OccaMem(),
                           constrList.OccaMem());
   }

   // FIXME: add case for HypreParMatrix here
   OccaConstrainedOperator *cA = dynamic_cast<OccaConstrainedOperator*>(A);
   if (cA)
   {
      cA->EliminateRHS(X.Get_PVector()->As<Vector>(),
                       B.Get_PVector()->As<Vector>());
   }
   else
   {
      mfem_error("OccaBilinearForm::InitRHS expects an OccaConstrainedOperator");
   }
}

// Matrix vector multiplication.
void OccaBilinearForm::Mult_(const Vector &x, Vector &y) const
{
   otrialFESpace->GlobalToLocal(x, localX);
   localY.Fill<double>(0.0);

   const int integratorCount = (int) integrators.size();
   for (int i = 0; i < integratorCount; ++i)
   {
      integrators[i]->MultAdd(localX, localY);
   }

   otestFESpace->LocalToGlobal(localY, y);
}

// Matrix transpose vector multiplication.
void OccaBilinearForm::MultTranspose_(const Vector &x, Vector &y) const
{
   otestFESpace->GlobalToLocal(x, localX);
   localY.Fill<double>(0.0);

   const int integratorCount = (int) integrators.size();
   for (int i = 0; i < integratorCount; ++i)
   {
      integrators[i]->MultTransposeAdd(localX, localY);
   }

   otrialFESpace->LocalToGlobal(localY, y);
}

void OccaBilinearForm::OccaRecoverFEMSolution(const mfem::Vector &X,
                                              const mfem::Vector &b,
                                              mfem::Vector &x)
{
   const mfem::Operator *P = this->GetTrialProlongation();
   if (P)
   {
      // Apply conforming prolongation
      x.Resize(P->OutLayout());
      P->Mult(X, x);
   }
   // Otherwise X and x point to the same data
}

// Frees memory bilinear form.
OccaBilinearForm::~OccaBilinearForm()
{
   // Make sure all integrators free their data
   IntegratorVector::iterator it = integrators.begin();
   while (it != integrators.end())
   {
      delete *it;
      ++it;
   }
}


void BilinearForm::InitOccaBilinearForm()
{
   // Init 'obform' using 'bform'
   MFEM_ASSERT(bform != NULL, "");
   MFEM_ASSERT(obform == NULL, "");

   FiniteElementSpace &ofes =
      bform->FESpace()->Get_PFESpace()->As<FiniteElementSpace>();
   obform = new OccaBilinearForm(&ofes);

   // Transfer domain integrators
   mfem::Array<mfem::BilinearFormIntegrator*> &dbfi = *bform->GetDBFI();
   for (int i = 0; i < dbfi.Size(); i++)
   {
      std::string integ_name(dbfi[i]->Name());
      Coefficient *scal_coeff = dbfi[i]->GetScalarCoefficient();
      ConstantCoefficient *const_coeff =
         dynamic_cast<ConstantCoefficient*>(scal_coeff);
      GridFunctionCoefficient *gridfunc_coeff =
         dynamic_cast<GridFunctionCoefficient*>(scal_coeff);
      // TODO: other types of coefficients ...

      const bool use_acrotensor = obform->OccaEngine().UseAcrotensorIntegrator();

      OccaCoefficient *ocoeff = NULL;
      if (const_coeff)
      {
         ocoeff = new OccaCoefficient(obform->OccaEngine(),
                                      const_coeff->constant);
      }
      else if (gridfunc_coeff)
      {
         ocoeff = new OccaCoefficient(obform->OccaEngine(),
                                      *gridfunc_coeff->GetGridFunction(), true);
      }
      else if (!scal_coeff)
      {
         ocoeff = new OccaCoefficient(obform->OccaEngine(), 1.0);
      }
      else
      {
         MFEM_ABORT("Coefficient type not supported");
      }

      OccaIntegrator *ointeg = NULL;
      if (integ_name == "(undefined)")
      {
         MFEM_ABORT("BilinearFormIntegrator does not define Name()");
      }
      else if (integ_name == "mass")
      {
         if (!use_acrotensor) {
            ointeg = new OccaMassIntegrator(*ocoeff);
         }
         else {
            ointeg = new ::mfem::acro::AcroMassIntegrator(obform->OccaEngine());
         }
      }
      else if (integ_name == "diffusion")
      {
         if (!use_acrotensor) {
            ointeg = new OccaDiffusionIntegrator(*ocoeff);
         }
         else {
            ointeg = new ::mfem::acro::AcroDiffusionIntegrator(obform->OccaEngine());
         }
      }
      else
      {
         MFEM_ABORT("BilinearFormIntegrator [Name() = " << integ_name
                    << "] is not supported");
      }

      // NOTE: The integrators copy ocoeff, so it can be deleted here so there
      //       is no memory leak.
      delete ocoeff;

      const mfem::IntegrationRule *ir = dbfi[i]->GetIntRule();
      if (ir) { ointeg->SetIntegrationRule(*ir); }

      obform->AddDomainIntegrator(ointeg);
   }

   // TODO: other types of integrators ...
}

bool BilinearForm::Assemble()
{
   if (obform == NULL) { InitOccaBilinearForm(); }

   obform->Assemble();

   return true; // --> host assembly is not needed
}

void BilinearForm::FormSystemMatrix(const mfem::Array<int> &ess_tdof_list,
                                    mfem::OperatorHandle &A)
{
   ::occa::properties props(A.GetSpec());
   std::string repr(props.get("representation", std::string("partial")));

   if (repr == "partial")
   {
      // ConstrainedOperator . RAP
      mfem::Operator *Aout = NULL;
      obform->FormOperator(ess_tdof_list, Aout);
      A.Reset(Aout);
   }
   else
   {
      // Assumes HYPRE backend

      // FIXME: serial mode is not supported
      mfem::ParFiniteElementSpace *pfespace = dynamic_cast<mfem::ParFiniteElementSpace*>(&obform->GetTrialFESpace());
      if (!pfespace) mfem_error("Not supported for now");

      // FIXME: Face terms are not supported
      // There doesn't exist a check for this at the moment.

      // Create engine
      // This performs various checks like whether the memory space that OCCA uses is compatible with HYPRE
      mfem::hypre::FiniteElementSpace hfes(*engine, *pfespace);

      // Make the E->E "matrix" operator (stored unrolled as a vector)
      const int num_elements = pfespace->GetNE();
      const int num_dofs_per_el = pfespace->GetFE(0)->GetDof() * pfespace->GetVDim();

      mfem::DenseTensor element_matrices(num_dofs_per_el, num_dofs_per_el, num_elements);
      obform->AssembleElementMatrices(element_matrices);

      const Table &elem_dof = pfespace->GetElementToDofTable();
      Table dof_dof;

      const int height = pfespace->GetVLayout()->Size();
      // the sparsity pattern is defined from the map: element->dof
      Table dof_elem;
      Transpose(elem_dof, dof_elem, height);
      mfem::Mult(dof_elem, elem_dof, dof_dof);

      dof_dof.SortRows();

      int *I = dof_dof.GetI();
      int *J = dof_dof.GetJ();
      double *data = new double[I[height]];

      SparseMatrix A_local(I, J, data, height, height, true, true, true);
      A_local = 0.0;

      dof_dof.LoseData();

      mfem::Array<int> vdofs;
      for (int e = 0; e < num_elements; e++)
      {
         pfespace->GetElementVDofs(e, vdofs);
         A_local.AddSubMatrix(vdofs, vdofs, element_matrices(e));
      }

      if (ess_tdof_list.Size() > 0) mfem_error("TBD");
      {
         // EliminateVDofs in A_local (ess_tdof_list, KEEP_DIAG);
      }

      A_local.Finalize(true);

      // Make the L->L matrix operator
      mfem::hypre::ParMatrix lmat(hfes.GetLLayout(), A_local);

      // Get the T->L matrix operator
      const mfem::hypre::ParMatrix &t_to_l = hfes.GetProlongation();

      // RAP it
      A.Reset(mfem::hypre::MakePtAP(t_to_l, lmat));

      // // TODO: Incorporate this method of using colors to improve the scalability of the serial loop above
      // // After implementing a sparse matrix on the device.
      // const mfem::Table &el_to_dof;
      // mfem::Table dof_to_el, el_to_el;
      // Transpose(el_to_dof, dof_to_el);
      // Mult(el_to_dof, dof_to_el, el_to_el);

      // // TODO: Get mesh
      // mfem::Array<int> el_to_color;
      // mesh->GetElementColoring(el_to_el, el_to_color);

      // mfem::Table color_to_el;
      // mfem::Transpose(el_to_color, color_to_el);
   }
}

void BilinearForm::FormLinearSystem(const mfem::Array<int> &ess_tdof_list,
                                    mfem::Vector &x, mfem::Vector &b,
                                    mfem::OperatorHandle &A,
                                    mfem::Vector &X, mfem::Vector &B,
                                    int copy_interior)
{
   FormSystemMatrix(ess_tdof_list, A);
   obform->InitRHS(ess_tdof_list, x, b, A.Ptr(), X, B, copy_interior);
}

void BilinearForm::RecoverFEMSolution(const mfem::Vector &X,
                                      const mfem::Vector &b,
                                      mfem::Vector &x)
{
   obform->OccaRecoverFEMSolution(X, b, x);
}

BilinearForm::~BilinearForm()
{
   delete obform;
}

} // namespace mfem::occa

} // namespace mfem

#endif // defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_OCCA)
