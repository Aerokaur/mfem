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

#ifdef MFEM_USE_ACROTENSOR
#ifndef MFEM_ACRODIFFUSIONINTEG
#define MFEM_ACRODIFFUSIONINTEG

#include "pabilininteg.hpp"
#include "AcroTensor.hpp"

namespace mfem {

class AcroDiffusionIntegrator : public PAIntegrator 
{
  private:
  acro::TensorEngine TE;
  int nDof1D;
  int nQuad1D;

  acro::Tensor B, G;         //Basis and dbasis evaluated on the quad points
  acro::Tensor W;            //Integration weights
  Array<acro::Tensor*> Btil; //Btilde used to compute stiffness matrix
  acro::Tensor D;            //Product of integration weight, physical consts, and element shape info
  acro::Tensor S;            //The assembled local stiffness matrices
  acro::Tensor U, Z, T1, T2; //Intermediate computations for tensor product partial assembly

  void ComputeBTilde();

public:
  AcroDiffusionIntegrator(Coefficient &q, FiniteElementSpace &f, bool gpu);
  virtual ~AcroDiffusionIntegrator();
  virtual void BatchedPartialAssemble();
  virtual void BatchedAssembleMatrix();
  virtual void PAMult(const Vector &x, Vector &y);
};

}

#endif
#endif
