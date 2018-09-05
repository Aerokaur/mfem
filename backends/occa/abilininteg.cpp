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
#if defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_ACROTENSOR) && defined(MFEM_USE_OCCA)

#include "abilininteg.hpp"
#include "../../fem/fem.hpp"
#include "occa/mode/cuda.hpp"

namespace mfem
{

namespace acro
{

PAIntegrator::PAIntegrator(const mfem::occa::Engine &engine) : OccaIntegrator(engine) {}

void PAIntegrator::Setup()
{
   onGPU = (engine->GetDevice(0).mode() == "CUDA");
   fe = trialFESpace->GetFE(0);
   tfe = dynamic_cast<const TensorBasisElement*>(fe);
   if (tfe)
   {
      tDofMap = tfe->GetDofMap();
   }
   else
   {
      tDofMap.SetSize(nDof);
      for (int i = 0; i < nDof; ++i)
      {
         tDofMap[i] = i;
      }
   }

   GeomType = fe->GetGeomType();
   FEOrder = fe->GetOrder();
   nDim    = fe->GetDim();
   nDof   = fe->GetDof();

   nQuad = ir->GetNPoints();

   ir1D = &IntRules.Get(Geometry::SEGMENT, ir->GetOrder());
   nDof1D = FEOrder + 1;
   nQuad1D = ir1D->GetNPoints();

   if (nDim > 3)
   {
      mfem_error("AcroIntegrator tensor computations don't support dim > 3.");
   }
}

AcroMassIntegrator::AcroMassIntegrator(const mfem::occa::Engine &engine) : PAIntegrator(engine) {}

void AcroMassIntegrator::Setup()
{
   // Populate base class members first
   PAIntegrator::Setup();

   const int nElem = trialFESpace->GetNE();

   if (hasTensorBasis)
   {
      H1_FECollection fec(FEOrder,1);
      const FiniteElement *fe1D = fec.FiniteElementForGeometry(Geometry::SEGMENT);
      Vector eval(nDof1D);
      DenseMatrix deval(nDof1D,1);
      B.Init(nQuad1D, nDof1D);
      std::vector<int> wdims(nDim, nQuad1D);
      W.Init(wdims);

      Vector w(nQuad1D);
      for (int k = 0; k < nQuad1D; ++k)
      {
         const IntegrationPoint &ip = ir1D->IntPoint(k);
         fe1D->CalcShape(ip, eval);

         B(k,0) = eval(0);
         B(k,nDof1D-1) = eval(1);
         for (int i = 1; i < nDof1D-1; ++i)
         {
            B(k,i) = eval(i+1);
         }
         w(k) = ip.weight;
      }

      if (nDim == 1)
      {
         for (int k1 = 0; k1 < nQuad1D; ++k1)
         {
            W(k1) = w(k1);
         }
      }
      else if (nDim == 2)
      {
         for (int k1 = 0; k1 < nQuad1D; ++k1)
         {
            for (int k2 = 0; k2 < nQuad1D; ++k2)
            {
               W(k1,k2) = w(k1)*w(k2);
            }
         }
      }
      else if (nDim == 3)
      {
         for (int k1 = 0; k1 < nQuad1D; ++k1)
         {
            for (int k2 = 0; k2 < nQuad1D; ++k2)
            {
               for (int k3 = 0; k3 < nQuad1D; ++k3)
               {
                  W(k1,k2,k3) = w(k1)*w(k2)*w(k3);
               }
            }
         }
      }
   }
   else
   {
      Vector eval(nDof);
      W.Init(nQuad);
      for (int k = 0; k < nQuad; ++k)
      {
         const IntegrationPoint &ip = ir->IntPoint(k);

         W(k) = ip.weight;
      }
   }

   if (onGPU)
   {
      TE.SetExecutorType("Cuda");
      ::acro::setCudaContext(::occa::cuda::getContext(engine->GetDevice(0)));
      B.MapToGPU();
      W.MapToGPU();
   }
   else
   {
      TE.SetExecutorType("CPUInterpreted");
   }

   if (hasTensorBasis)
   {
      if (nDim == 1)
      {
         D.Init(nElem, nQuad1D);
         T1.Init(nElem, nQuad1D);
         if (onGPU) {
            T1.SwitchToGPU();
         }
      }
      else if (nDim == 2)
      {
         D.Init(nElem, nQuad1D, nQuad1D);
         T1.Init(nElem, nQuad1D, nDof1D);
         T2.Init(nElem, nQuad1D, nQuad1D);
         if (onGPU) {
            T1.SwitchToGPU();
            T2.SwitchToGPU();
         }
      }
      else if (nDim == 3)
      {
         D.Init(nElem, nQuad1D, nQuad1D, nQuad1D);
         T1.Init(nElem, nQuad1D, nDof1D, nDof1D);
         T2.Init(nElem, nQuad1D, nQuad1D, nDof1D);
         T3.Init(nElem, nQuad1D, nQuad1D, nQuad1D);
         if (onGPU) {
            T1.SwitchToGPU();
            T2.SwitchToGPU();
            T3.SwitchToGPU();
         }
      }
      else
      {
         mfem_error("AcroDiffusionIntegrator tensor computations don't support dim > 3.");
      }
   }
   else
   {
      D.Init(nElem, nQuad);
   }
}

void AcroMassIntegrator::BatchedPartialAssemble()
{
   const int nElem = trialFESpace->GetNE();

   //Initilze the tensors
   ::acro::Tensor J,Jdet,q;
   if (hasTensorBasis)
   {
      if (nDim == 1)
      {
         J.Init(nElem, nQuad1D, nQuad1D, nDim, nDim);
         Jdet.Init(nElem, nQuad1D);
         q.Init(nElem, nQuad1D);
      }
      else if (nDim == 2)
      {
         J.Init(nElem, nQuad1D, nQuad1D, nDim, nDim);
         Jdet.Init(nElem, nQuad1D, nQuad1D);
         q.Init(nElem, nQuad1D, nQuad1D);
      }
      else if (nDim == 3)
      {
         J.Init(nElem, nQuad1D, nQuad1D, nQuad1D, nDim, nDim);
         Jdet.Init(nElem, nQuad1D, nQuad1D, nQuad1D);
         q.Init(nElem, nQuad1D, nQuad1D,nQuad1D);
      }
      else
      {
         mfem_error("AcroDiffusionIntegrator tensor computations don't support dim > 3.");
      }
   }
   else
   {
      J.Init(nElem, nQuad, nDim, nDim);
      Jdet.Init(nElem, nQuad);
      q.Init(nElem, nQuad);
   }

   //Fill the jacobians and coefficients
   int idx = 0;
   for (int e = 0; e < nElem; ++e)
   {
      ElementTransformation *Trans = trialFESpace->GetElementTransformation(e);
      for (int k = 0; k < nQuad; ++k)
      {
         const IntegrationPoint &ip = ir->IntPoint(k);
         Trans->SetIntPoint(&ip);
         q[e*nQuad+k] = 1.0;
         const DenseMatrix &JMat = Trans->Jacobian();
         for (int m = 0; m < nDim; ++m)
         {
            for (int n = 0; n < nDim; ++n)
            {
               J[idx] = JMat(m,n);
               idx ++;
            }
         }
      }
   }
   TE.BatchMatrixDet(Jdet, J);

   if (hasTensorBasis)
   {
      if (nDim == 1) {
         TE("D_e_k = W_k Q_e_k Jdet_e_k",
            D, W, q, Jdet);
      }
      else if (nDim == 2)
      {
         TE("D_e_k1_k2 = W_k1_k2 Q_e_k1_k2 Jdet_e_k1_k2",
            D, W, q, Jdet);
      }
      else if (nDim == 3)
      {
         TE("D_e_k1_k2_k3 = W_k1_k2_k3 Q_e_k1_k2_k3 Jdet_e_k1_k2_k3",
            D, W, q, Jdet);
      }
      else
      {
         mfem_error("AcroMassIntegrator tensor computations don't support dim > 3.");
      }
   }
   else
   {
      TE("D_e_k = W_k Q_e_k Jdet_e_k",
         D, W, q, Jdet);
   }
}

void AcroMassIntegrator::BatchedAssembleElementMatrices(DenseTensor &elmats)
{
   const int nElem = trialFESpace->GetNE();

   if (!D.IsInitialized())
   {
      BatchedPartialAssemble();
   }

   if (!M.IsInitialized())
   {
      if (hasTensorBasis)
      {
         if (nDim == 1)
         {
            M.Init(nElem, nDof1D, nDof1D);
         }
         else if (nDim == 2)
         {
            M.Init(nElem, nDof1D, nDof1D, nDof1D, nDof1D);
         }
         else if (nDim == 3)
         {
            M.Init(nElem, nDof1D, nDof1D, nDof1D, nDof1D, nDof1D, nDof1D);
         }
      }
      else
      {
         M.Init(nElem, nDof, nDof);
      }
      if (onGPU) {M.SwitchToGPU();}
   }

   if (hasTensorBasis) {
      if (nDim == 1) {
         TE("M_e_i1_j1 = B_k1_i1 B_k1_j1 D_e_k1",
            M, B, B, D);
      }
      else if (nDim == 2)
      {
         TE("M_e_i1_i2_j1_j2 = B_k1_i1 B_k1_j1 B_k2_i2 B_k2_j2 D_e_k1_k2",
            M, B, B, B, B, D);
      }
      else if (nDim == 3)
      {
         TE("M_e_i1_i2_i3_j1_j2_j3 = B_k1_i1 B_k1_j1 B_k2_i2 B_k2_j2 B_k3_i3 B_k3_j3 D_e_k1_k2_k3",
            M, B, B, B, B, B, B, D);
      }
   }
   else
   {
      TE("M_e_i_j = B_k_i B_k_j D_e_k", M, B, B, D);
   }

   M.MoveFromGPU();
   for (int e = 0; e < nElem; ++e)
   {
      for (int ei = 0; ei < nDof; ++ei)
      {
         for (int ej = 0; ej < nDof; ++ej)
         {
            elmats(tDofMap[ei], tDofMap[ej], e) = M[e*nDof*nDof + ei*nDof + ej];
         }
      }
   }

}

void AcroMassIntegrator::SetupIntegrationRule() {
   const mfem::FiniteElement &trialFE = *(trialFESpace->GetFE(0));
   const mfem::FiniteElement &testFE  = *(testFESpace->GetFE(0));
   mfem::ElementTransformation &T = *trialFESpace->GetElementTransformation(0);
   ir = &mfem::MassIntegrator::GetRule(trialFE, testFE, T);
}

void AcroMassIntegrator::MultAdd(mfem::occa::Vector &x, mfem::occa::Vector &y)
{
   const int nElem = trialFESpace->GetNE();

   double *x_ptr = (double*)x.OccaMem().ptr();
   double *y_ptr = (double*)y.OccaMem().ptr();
   if (hasTensorBasis)
   {
      if (nDim == 1)
      {
         ::acro::Tensor X(nElem, nDof1D, x_ptr, x_ptr, onGPU);
         ::acro::Tensor Y(nElem, nDof1D, y_ptr, y_ptr, onGPU);
         TE("T1_e_k1 = D_e_k1 B_k1_j1 X_e_j1", T1, D, B, X);
         TE("Y_e_i1 = B_k1_i1 T1_e_k1", Y, B, T1);
      }
      else if (nDim == 2)
      {
         ::acro::Tensor X(nElem, nDof1D, nDof1D, x_ptr, x_ptr, onGPU);
         ::acro::Tensor Y(nElem, nDof1D, nDof1D, y_ptr, y_ptr, onGPU);
         TE("T1_e_k2_j1 = B_k2_j2 X_e_j1_j2", T1, B, X);
         TE("T2_e_k1_k2 = D_e_k1_k2 B_k1_j1 T1_e_k2_j1", T2, D, B, T1);
         TE("T1_e_k1_i2 = B_k2_i2 T2_e_k1_k2", T1, B, T2);
         TE("Y_e_i1_i2 = B_k1_i1 T1_e_k1_i2", Y, B, T1);
      }
      else if (nDim == 3)
      {
         ::acro::Tensor X(nElem, nDof1D, nDof1D, nDof1D, x_ptr, x_ptr, onGPU);
         ::acro::Tensor Y(nElem, nDof1D, nDof1D, nDof1D, y_ptr, y_ptr, onGPU);
         TE("T1_e_k3_j1_j2 = B_k3_j3 X_e_j1_j2_j3", T1, B, X);
         TE("T2_e_k2_k3_j1 = B_k2_j2 T1_e_k3_j1_j2", T2, B, T1);
         TE("T3_e_k1_k2_k3 = D_e_k1_k2_k3 B_k1_j1 T2_e_k2_k3_j1", T3, D, B, T2);
         TE("T2_e_k1_k2_i3 = B_k3_i3 T3_e_k1_k2_k3", T2, B, T3);
         TE("T1_e_k1_i2_i3 = B_k2_i2 T2_e_k1_k2_i3", T1, B, T2);
         TE("Y_e_i1_i2_i3 = B_k1_i1 T1_e_k1_i2_i3", Y, B, T1);
      }
   }
   else
   {
      mfem_error("AcroMassIntegrator PAMult on simplices not supported");
   }
}


AcroDiffusionIntegrator::AcroDiffusionIntegrator(const mfem::occa::Engine &engine) : PAIntegrator(engine) {}

void AcroDiffusionIntegrator::Setup()
{
   // Populate base class members first
   PAIntegrator::Setup();

   const int nElem = trialFESpace->GetNE();

   if (hasTensorBasis)
   {
      H1_FECollection fec(FEOrder,1);
      const FiniteElement *fe1D = fec.FiniteElementForGeometry(Geometry::SEGMENT);
      Vector eval(nDof1D);
      DenseMatrix deval(nDof1D,1);
      B.Init(nQuad1D, nDof1D);
      G.Init(nQuad1D, nDof1D);
      std::vector<int> wdims(nDim, nQuad1D);
      W.Init(wdims);

      Vector w(nQuad1D);
      for (int k = 0; k < nQuad1D; ++k)
      {
         const IntegrationPoint &ip = ir1D->IntPoint(k);
         fe1D->CalcShape(ip, eval);
         fe1D->CalcDShape(ip, deval);

         B(k,0) = eval(0);
         B(k,nDof1D-1) = eval(1);
         G(k,0) = deval(0,0);
         G(k,nDof1D-1) = deval(1,0);
         for (int i = 1; i < nDof1D-1; ++i)
         {
            B(k,i) = eval(i+1);
            G(k,i) = deval(i+1,0);
         }
         w(k) = ip.weight;
      }

      if (nDim == 1)
      {
         for (int k1 = 0; k1 < nQuad1D; ++k1)
         {
            W(k1) = w(k1);
         }
      }
      else if (nDim == 2)
      {
         for (int k1 = 0; k1 < nQuad1D; ++k1)
         {
            for (int k2 = 0; k2 < nQuad1D; ++k2)
            {
               W(k1,k2) = w(k1)*w(k2);
            }
         }
      }
      else if (nDim == 3)
      {
         for (int k1 = 0; k1 < nQuad1D; ++k1)
         {
            for (int k2 = 0; k2 < nQuad1D; ++k2)
            {
               for (int k3 = 0; k3 < nQuad1D; ++k3)
               {
                  W(k1,k2,k3) = w(k1)*w(k2)*w(k3);
               }
            }
         }
      }
   }
   else
   {
      Vector eval(nDof);
      DenseMatrix deval(nDof,nDim);
      G.Init(nQuad, nDof,nDim);
      W.Init(nQuad);
      for (int k = 0; k < nQuad; ++k)
      {
         const IntegrationPoint &ip = ir->IntPoint(k);
         fe->CalcDShape(ip, deval);
         for (int i = 0; i < nDof; ++i)
         {
            for (int d = 0; d < nDim; ++d)
            {
               G(k,i,d) = deval(i,d);
            }
         }
         W(k) = ip.weight;
      }
   }

   if (onGPU)
   {
      //TE.SetExecutorType("OneOutPerThread");
      ::acro::setCudaContext(::occa::cuda::getContext(engine->GetDevice(0)));
      TE.SetExecutorType("Cuda");
      //TODO:  Set to an existing cuda context if one exists
      B.MapToGPU();
      G.MapToGPU();
      W.MapToGPU();
   }
   else
   {
      TE.SetExecutorType("CPUInterpreted");
   }

   if (hasTensorBasis)
   {
      if (nDim == 1)
      {
         D.Init(nElem, nDim, nDim, nQuad1D);
         U.Init(nDim, nElem, nQuad1D);
         Z.Init(nDim, nElem, nQuad1D);
         if (onGPU)
         {
            U.SwitchToGPU();
            Z.SwitchToGPU();
         }
      }
      else if (nDim == 2)
      {
         D.Init(nElem, nDim, nDim, nQuad1D, nQuad1D);
         U.Init(nDim, nElem, nQuad1D, nQuad1D);
         Z.Init(nDim, nElem, nQuad1D, nQuad1D);
         T1.Init(nElem,nDof1D,nQuad1D);
         if (onGPU)
         {
            U.SwitchToGPU();
            Z.SwitchToGPU();
            T1.SwitchToGPU();
         }
      }
      else if (nDim == 3)
      {
         D.Init(nElem, nDim, nDim, nQuad1D, nQuad1D, nQuad1D);
         U.Init(nDim, nElem, nQuad1D, nQuad1D, nQuad1D);
         Z.Init(nDim, nElem, nQuad1D, nQuad1D, nQuad1D);
         T1.Init(nElem, nDof1D, nQuad1D, nQuad1D);
         T2.Init(nElem, nDof1D, nDof1D, nQuad1D);
         if (onGPU)
         {
            U.SwitchToGPU();
            Z.SwitchToGPU();
            T1.SwitchToGPU();
            T2.SwitchToGPU();
         }
      }
      else
      {
         mfem_error("AcroIntegrator tensor computations don't support dim > 3.");
      }
   }
   else
   {
      D.Init(nElem, nDim, nDim, nQuad);
   }

}

void AcroDiffusionIntegrator::SetupIntegrationRule()
{
   const FiniteElement &trialFE = *(trialFESpace->GetFE(0));
   const FiniteElement &testFE  = *(testFESpace->GetFE(0));
   ir = &mfem::DiffusionIntegrator::GetRule(trialFE, testFE);
}

void AcroDiffusionIntegrator::ComputeBTilde()
{
   Btil.SetSize(nDim);
   for (int d = 0; d < nDim; ++d)
   {
      Btil[d] = new ::acro::Tensor(nDim, nDim, nQuad1D, nDof1D, nDof1D);
      for (int m = 0; m < nDim; ++m)
      {
         for (int n = 0; n < nDim; ++n)
         {
            ::acro::Tensor &BGM = (m == d) ? G : B;
            ::acro::Tensor &BGN = (n == d) ? G : B;
            for (int k = 0; k < nQuad1D; ++k)
            {
               for (int i = 0; i < nDof1D; ++i)
               {
                  for (int j = 0; j < nDof1D; ++j)
                  {
                     (*Btil[d])(m, n, k, i, j) = BGM(k,i)*BGN(k,j);
                  }
               }
            }
         }
      }
   }
}

void AcroDiffusionIntegrator::BatchedPartialAssemble()
{
   const int nElem = trialFESpace->GetNE();

   //Initilze the tensors
   ::acro::Tensor J,Jinv,Jdet,C;
   if (hasTensorBasis)
   {
      const IntegrationRule *ir1D = &IntRules.Get(Geometry::SEGMENT, ir->GetOrder());
      IntegrationPoint ip;
      if (nDim == 1)
      {
         J.Init(nElem, nQuad1D, nDim, nDim);
         Jinv.Init(nElem, nQuad1D, nDim, nDim);
         Jdet.Init(nElem, nQuad1D);
         C.Init(nElem, nQuad1D);

         for (int e = 0; e < nElem; ++e)
         {
            ElementTransformation *Trans = trialFESpace->GetElementTransformation(e);
            for (int k1 = 0; k1 < nQuad1D; ++k1)
            {
               ip.x = ir1D->IntPoint(k1).x;
               ip.y = 0.0;
               ip.z = 0.0;
               Trans->SetIntPoint(&ip);
               C(e,k1) = 1.0; // Q->Eval(*Trans, ip);
               const DenseMatrix &JMat = Trans->Jacobian();
               for (int m = 0; m < nDim; ++m)
               {
                  for (int n = 0; n < nDim; ++n)
                  {
                     J(e,k1,m,n) = JMat.Elem(m,n);
                  }
               }
            }
         }
      }
      else if (nDim == 2)
      {
         J.Init(nElem, nQuad1D, nQuad1D, nDim, nDim);
         Jinv.Init(nElem, nQuad1D, nQuad1D, nDim, nDim);
         Jdet.Init(nElem, nQuad1D, nQuad1D);
         C.Init(nElem, nQuad1D, nQuad1D);

         for (int e = 0; e < nElem; ++e)
         {
            ElementTransformation *Trans = trialFESpace->GetElementTransformation(e);
            for (int k1 = 0; k1 < nQuad1D; ++k1)
            {
               for (int k2 = 0; k2 < nQuad1D; ++k2)
               {
                  ip.x = ir1D->IntPoint(k1).x;
                  ip.y = ir1D->IntPoint(k2).x;
                  ip.z = 0.0;
                  Trans->SetIntPoint(&ip);
                  C(e,k1,k2) = 1.0; //Q->Eval(*Trans, ip);
                  const DenseMatrix &JMat = Trans->Jacobian();
                  for (int m = 0; m < nDim; ++m)
                  {
                     for (int n = 0; n < nDim; ++n)
                     {
                        J(e,k1,k2,m,n) = JMat.Elem(m,n);
                     }
                  }
               }
            }
         }
      }
      else if (nDim == 3)
      {
         J.Init(nElem, nQuad1D, nQuad1D, nQuad1D, nDim, nDim);
         Jinv.Init(nElem, nQuad1D, nQuad1D, nQuad1D, nDim, nDim);
         Jdet.Init(nElem, nQuad1D, nQuad1D, nQuad1D);
         C.Init(nElem, nQuad1D, nQuad1D, nQuad1D);

         for (int e = 0; e < nElem; ++e)
         {
            ElementTransformation *Trans = trialFESpace->GetElementTransformation(e);
            for (int k1 = 0; k1 < nQuad1D; ++k1)
            {
               for (int k2 = 0; k2 < nQuad1D; ++k2)
               {
                  for (int k3 = 0; k3 < nQuad1D; ++k3)
                  {
                     ip.x = ir1D->IntPoint(k1).x;
                     ip.y = ir1D->IntPoint(k2).x;
                     ip.z = ir1D->IntPoint(k3).x;
                     Trans->SetIntPoint(&ip);
                     C(e,k1,k2,k3) = 1.0; //Q->Eval(*Trans, ip);
                     const DenseMatrix &JMat = Trans->Jacobian();
                     for (int m = 0; m < nDim; ++m)
                     {
                        for (int n = 0; n < nDim; ++n)
                        {
                           J(e,k1,k2,k3,m,n) = JMat.Elem(m,n);
                        }
                     }
                  }
               }
            }
         }
      }
   }
   else
   {
      J.Init(nElem, nQuad, nDim, nDim);
      Jinv.Init(nElem, nQuad, nDim, nDim);
      Jdet.Init(nElem, nQuad);
      C.Init(nElem, nQuad);

      for (int e = 0; e < nElem; ++e)
      {
         ElementTransformation *Trans = trialFESpace->GetElementTransformation(e);
         for (int k = 0; k < nQuad; ++k)
         {
            const IntegrationPoint &ip = ir->IntPoint(k);
            Trans->SetIntPoint(&ip);
            C(e,k) = 1.0; // Q->Eval(*Trans, ip);
            const DenseMatrix &JMat = Trans->Jacobian();
            for (int m = 0; m < nDim; ++m)
            {
               for (int n = 0; n < nDim; ++n)
               {
                  J(e,k,m,n) = JMat.Elem(m,n);
               }
            }
         }
      }
   }

   TE.BatchMatrixInvDet(Jinv, Jdet, J);

   if (hasTensorBasis)
   {
      if (nDim == 1)
      {
         TE("D_e_m_n_k = W_k C_e_k Jdet_e_k Jinv_e_k_m_j Jinv_e_k_n_j",
            D, W, C, Jdet, Jinv, Jinv);
      }
      else if (nDim == 2)
      {
         TE("D_e_m_n_k1_k2 = W_k1_k2 C_e_k1_k2 Jdet_e_k1_k2 Jinv_e_k1_k2_m_j Jinv_e_k1_k2_n_j",
            D, W, C, Jdet, Jinv, Jinv);
      }
      else if (nDim == 3)
      {
         TE("D_e_m_n_k1_k2_k3 = W_k1_k2_k3 C_e_k1_k2_k3 Jdet_e_k1_k2_k3 Jinv_e_k1_k2_k3_n_j Jinv_e_k1_k2_k3_m_j",
            D, W, C, Jdet, Jinv, Jinv);
      }
   }
   else
   {
      TE("D_e_m_n_k = W_k C_e_k Jdet_e_k Jinv_e_k_m_j Jinv_e_k_n_j",
         D, W, C, Jdet, Jinv, Jinv);
   }
}


void AcroDiffusionIntegrator::BatchedAssembleElementMatrices(DenseTensor &elmats)
{
   const int nElem = trialFESpace->GetNE();

   if (hasTensorBasis && Btil.Size() == 0)
   {
      ComputeBTilde();
   }

   if (!D.IsInitialized())
   {
      BatchedPartialAssemble();
   }

   if (!S.IsInitialized())
   {
      if (hasTensorBasis)
      {
         if (nDim == 1)
         {
            S.Init(nElem, nDof1D, nDof1D);
         }
         else if (nDim == 2)
         {
            S.Init(nElem, nDof1D, nDof1D, nDof1D, nDof1D);
         }
         else if (nDim == 3)
         {
            S.Init(nElem, nDof1D, nDof1D, nDof1D, nDof1D, nDof1D, nDof1D);
         }
      }
      else
      {
         S.Init(nElem, nDof, nDof);
      }
      if (onGPU) {S.SwitchToGPU();}
   }


   if (hasTensorBasis) {
      if (nDim == 1) {
         TE("S_e_i1_j1 = Btil_m_n_k1_i1_j1 D_e_m_n_k1",
            S, *Btil[0], D);
      }
      else if (nDim == 2)
      {
         TE("S_e_i1_i2_j1_j2 = Btil1_m_n_k1_i1_j1 Btil2_m_n_k2_i2_j2 D_e_m_n_k1_k2",
            S, *Btil[0], *Btil[1], D);
      }
      else if (nDim == 3)
      {
         TE("S_e_i1_i2_i3_j1_j2_j3 = Btil1_m_n_k1_i1_j1 Btil2_m_n_k2_i2_j2 Btil3_m_n_k3_i3_j3 D_e_m_n_k1_k2_k3",
            S, *Btil[0], *Btil[1], *Btil[2], D);
      }
   }
   else
   {
      TE("S_e_i_j = G_k_i_m G_k_i_n D_e_m_n_k",
         S, G, G, D);
   }

   S.MoveFromGPU();
   for (int e = 0; e < nElem; ++e)
   {
      for (int ei = 0; ei < nDof; ++ei)
      {
         for (int ej = 0; ej < nDof; ++ej)
         {
            elmats(tDofMap[ei], tDofMap[ej], e) = S[e*nDof*nDof + ei*nDof + ej];
         }
      }
   }
}

void AcroDiffusionIntegrator::MultAdd(mfem::occa::Vector &x, mfem::occa::Vector &y)
{
   MFEM_ASSERT(hasTensorBasis,"AcroDiffusionIntegrator PAMult on simplices not supported");

   const int nElem = trialFESpace->GetNE();

   double *x_ptr = (double*)x.OccaMem().ptr();
   double *y_ptr = (double*)y.OccaMem().ptr();
   if (nDim == 1)
   {
      ::acro::Tensor X(nElem, nDof1D, x_ptr, x_ptr, onGPU);
      ::acro::Tensor Y(nElem, nDof1D, y_ptr, y_ptr, onGPU);

      TE("U_n_e_k1 = G_k1_i1 X_e_i1", U, G, X);
      TE("Z_m_e_k1 = D_e_m_n_k1 U_n_e_k1", Z, D, U);
      TE("Y_e_i1 = G_k1_i1 Z_m_e_k1", Y, G, Z);
   }
   else if (nDim == 2)
   {
      ::acro::Tensor X(nElem, nDof1D, nDof1D, x_ptr, x_ptr, onGPU);
      ::acro::Tensor Y(nElem, nDof1D, nDof1D, y_ptr, y_ptr, onGPU);

      ::acro::SliceTensor U1, U2, Z1, Z2;

      U1.SliceInit(U, 0); U2.SliceInit(U, 1);
      Z1.SliceInit(Z, 0); Z2.SliceInit(Z, 1);

      //U1_e_k1_k2 = G_k1_i1 B_k2_i2 X_e_i1_i2
      TE("BX_e_i1_k2 = B_k2_i2 X_e_i2_i1", T1, B, X);
      TE("U1_e_k1_k2 = G_k1_i1 BX_e_i1_k2", U1, G, T1);

      //U2_e_k1_k2 = B_k1_i1 G_k2_i2 X_e_i1_i2
      TE("GX_e_i1_k2 = G_k2_i2 X_e_i2_i1", T1, G, X);
      TE("U2_e_k1_k2 = B_k1_i1 GX_e_i1_k2", U2, B, T1);

      TE("Z_m_e_k1_k2 = D_e_m_n_k1_k2 U_n_e_k1_k2", Z, D, U);

      //Y_e_i1_i2 = G_k1_i1 B_k2_i2 Z1_e_k1_k2
      TE("BZ1_e_i2_k1 = B_k2_i2 Z1_e_k1_k2", T1, B, Z1);
      TE("Y_e_i2_i1 = G_k1_i1 BZ1_e_i2_k1", Y, G, T1);

      //Y_e_i1_i2 += B_k1_i1 G_k2_i2 Z2_e_k1_k2
      TE("GZ2_e_i2_k1 = G_k2_i2 Z2_e_k1_k2", T1, G, Z2);
      TE("Y_e_i2_i1 += B_k1_i1 GZ2_e_i2_k1", Y, B, T1);
   }
   else if (nDim == 3)
   {
      ::acro::Tensor X(nElem, nDof1D, nDof1D, nDof1D, x_ptr, x_ptr, onGPU);
      ::acro::Tensor Y(nElem, nDof1D, nDof1D, nDof1D, y_ptr, y_ptr, onGPU);

      ::acro::SliceTensor U1, U2, U3, Z1, Z2, Z3;

      U1.SliceInit(U, 0); U2.SliceInit(U, 1); U3.SliceInit(U, 2);
      Z1.SliceInit(Z, 0); Z2.SliceInit(Z, 1); Z3.SliceInit(Z, 2);

      TE.BeginMultiKernelLaunch();
      //U1_e_k1_k2_k3 = G_k1_i1 B_k2_i2 B_k3_i3 X_e_i1_i2_i3
      TE("T2_e_i1_i2_k3 = B_k3_i3 X_e_i1_i2_i3", T2, B, X);
      TE("T1_e_i1_k2_k3 = B_k2_i2 T2_e_i1_i2_k3", T1, B, T2);
      TE("U1_e_k1_k2_k3 = G_k1_i1 T1_e_i1_k2_k3", U1, G, T1);

      //U2_e_k1_k2_k3 = B_k1_i1 G_k2_i2 B_k3_i3 X_e_i1_i2_i3
      TE("T1_e_i1_k2_k3 = G_k2_i2 T2_e_i1_i2_k3", T1, G, T2);
      TE("U2_e_k1_k2_k3 = B_k1_i1 T1_e_i1_k2_k3", U2, B, T1);

      //U3_e_k1_k2_k3 = B_k1_i1 B_k2_i2 G_k3_i3 X_e_i1_i2_i3
      TE("T2_e_i1_i2_k3 = G_k3_i3 X_e_i1_i2_i3", T2, G, X);
      TE("T1_e_i1_k2_k3 = B_k2_i2 T2_e_i1_i2_k3", T1, B, T2);
      TE("U3_e_k1_k2_k3 = B_k1_i1 T1_e_i1_k2_k3", U3, B, T1);

      TE("Z_m_e_k1_k2_k3 = D_e_m_n_k1_k2_k3 U_n_e_k1_k2_k3", Z, D, U);

      //Y_e_i1_i2_i3 =  G_k1_i1 B_k2_i2 B_k3_i3 Z1_e_k1_k2_k3
      TE("T1_e_i3_k1_k2 = B_k3_i3 Z1_e_k1_k2_k3", T1, B, Z1);
      TE("T2_e_i2_i3_k1 = B_k2_i2 T1_e_i3_k1_k2", T2, B, T1);
      TE("Y_e_i1_i2_i3 = G_k1_i1 T2_e_i2_i3_k1", Y, G, T2);

      //Y_e_i1_i2_i3 +=  B_k1_i1 G_k2_i2 B_k3_i3 Z2_e_k1_k2_k3
      TE("T1_e_i3_k1_k2 = B_k3_i3 Z2_e_k1_k2_k3", T1, B, Z2);
      TE("T2_e_i2_i3_k1 = G_k2_i2 T1_e_i3_k1_k2", T2, G, T1);
      TE("Y_e_i1_i2_i3 += B_k1_i1 T2_e_i2_i3_k1", Y, B, T2);

      //Y_e_i1_i2_i3 +=  B_k1_i1 B_k2_i2 G_k3_i3 Z3_e_k1_k2_k3
      TE("T1_e_i3_k1_k2 = G_k3_i3 Z3_e_k1_k2_k3", T1, G, Z3);
      TE("T2_e_i2_i3_k1 = B_k2_i2 T1_e_i3_k1_k2", T2, B, T1);
      TE("Y_e_i1_i2_i3 += B_k1_i1 T2_e_i2_i3_k1", Y, B, T2);
      TE.EndMultiKernelLaunch();
   }
}

} // namespace mfem::acro

} // namespace mfem

#endif // defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_ACROTENSOR) && defined(MFEM_USE_OCCA)
