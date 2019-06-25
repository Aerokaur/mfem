#include "mfem.hpp"
#include "vec_conv_integrator.hpp"
#include <fstream>

using namespace std;
using namespace mfem;

enum PROB_TYPE
{
   MMS,
   KOV,
   LDC,
   CYL,
   THREEDCYL,
};

struct OptionSet
{
   PROB_TYPE prob_type;
   double rey;
   int vel_order;
   int print_level;
} opt_;

void vel_ex(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);

   u(0) = -cos(M_PI * xi) * sin(M_PI * yi);
   u(1) = sin(M_PI * xi) * cos(M_PI * yi);
}

void vel_cyl(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);

   double U = 0.3;

   if (xi <= 1e-8)
   {
      u(0) = 4.0 * U * yi * (0.41 - yi) / (pow(0.41, 2.0));
   }
   else
   {
      u(0) = 0.0;
   }
   u(1) = 0.0;
}

void vel_threedcyl(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);
   double zi = x(2);

   double U = 0.45;

   if (xi <= 1e-8)
   {
      u(0) = 16.0 * U * yi * zi * (0.41 - yi) * (0.41 - zi) / pow(0.41, 4.0);
   }
   else
   {
      u(0) = 0.0;
   }
   u(1) = 0.0;
   u(2) = 0.0;
}

void vel_ldc(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);

   if (yi > 1.0 - 1e-8)
   {
      u(0) = 4.0 * xi * (1.0 - xi);
   }
   else
   {
      u(0) = 0.0;
   }
   u(1) = 0.0;
}

double p_ex(const Vector &x)
{
   double xi = x(0);
   double yi = x(1);

   return xi + yi - 1.0;
}

void ffun(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);

   u(0) = 1.0 - 0.5 * M_PI * sin(2.0 * M_PI * xi) - 2.0 / opt_.rey * pow(M_PI,
                                                                         2.0) * cos(M_PI * xi) * sin(M_PI * yi);
   u(1) = 1.0 - 0.5 * M_PI * sin(2.0 * M_PI * yi) + 2.0 / opt_.rey * pow(M_PI,
                                                                         2.0) * cos(M_PI * yi) * sin(M_PI * xi);
}

double kov_lam()
{
   return opt_.rey / 2.0 - sqrt(pow(opt_.rey, 2.0) / 4.0 + 4.0 * pow(M_PI, 2.0));
}

void kov_vel_ex(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);

   double lam = kov_lam();

   u(0) = 1.0 - exp(lam * xi) * cos(2.0 * M_PI * yi);
   u(1) = lam / (2.0 * M_PI) * exp(lam * xi) * sin(2.0 * M_PI * yi);
}

double kov_p_ex(const Vector &x)
{
   double xi = x(0);

   double lam = kov_lam();

   return 1.0 - 1.0 / 2.0 * exp(2.0 * lam * xi);
}

class NavierStokesOperator : public Operator
{
private:
   ParMesh *pmesh_;
   Array<ParFiniteElementSpace *> fes_;
   Array<int> ess_bdr_attr_;
   Array<int> ess_tdof_list_;

   Array<int> block_offsets_;
   Array<int> block_trueOffsets_;
   BlockVector x, rhs;
   BlockVector trueX, trueRhs;

   VectorGridFunctionCoefficient vel_fc;
   ParNonlinearForm *N;
   ParBilinearForm *sform;
   ParLinearForm *fform;
   ParBilinearForm *mpform;
   ParMixedBilinearForm *dform;

   HypreParMatrix *S;
   HypreParMatrix *Mp;
   HypreParMatrix *D;
   HypreParMatrix *G;
   mutable HypreParMatrix *NjacS;

   BlockOperator *jac;
   BlockOperator *lin;

   HypreSolver *invS;
   HypreSolver *invMp;
   BlockDiagonalPreconditioner *stokesprec;
   IterativeSolver *jac_solver;

   NewtonSolver newton_solver;

   ParGridFunction *vel_gf;
   ParGridFunction *p_gf;

public:
   NavierStokesOperator(Array<ParFiniteElementSpace *> &fes) : Operator(
         fes[0]->TrueVSize() + fes[1]->TrueVSize()),
      pmesh_(fes[0]->GetParMesh()), fes_(fes),
      ess_bdr_attr_(pmesh_->bdr_attributes.Max()),
      N(nullptr),
      S(nullptr), Mp(nullptr), D(nullptr), G(nullptr), NjacS(nullptr),
      jac(nullptr), lin(nullptr), invS(nullptr), invMp(nullptr),
      stokesprec(nullptr), jac_solver(nullptr),
      newton_solver(pmesh_->GetComm()), vel_gf(nullptr)
   {
      if (opt_.prob_type == PROB_TYPE::KOV ||
          opt_.prob_type == PROB_TYPE::MMS ||
          opt_.prob_type == PROB_TYPE::LDC)
      {
         ess_bdr_attr_ = 1;
      }
      else if (opt_.prob_type == PROB_TYPE::CYL)
      {
         ess_bdr_attr_[0] = 1;
         ess_bdr_attr_[1] = 1;
         ess_bdr_attr_[2] = 1;
         ess_bdr_attr_[3] = 0;
      }
      else if (opt_.prob_type == PROB_TYPE::THREEDCYL)
      {
         ess_bdr_attr_[0] = 1;
         ess_bdr_attr_[1] = 0;
         ess_bdr_attr_[2] = 1;
      }

      fes_[0]->GetEssentialTrueDofs(ess_bdr_attr_, ess_tdof_list_);

      block_offsets_.SetSize(3);
      block_offsets_[0] = 0;
      block_offsets_[1] = fes[0]->GetVSize();
      block_offsets_[2] = fes[1]->GetVSize();
      block_offsets_.PartialSum();

      block_trueOffsets_.SetSize(3);
      block_trueOffsets_[0] = 0;
      block_trueOffsets_[1] = fes[0]->TrueVSize();
      block_trueOffsets_[2] = fes[1]->TrueVSize();
      block_trueOffsets_.PartialSum();

      x.Update(block_offsets_);
      rhs.Update(block_offsets_);

      trueX.Update(block_trueOffsets_);
      trueRhs.Update(block_trueOffsets_);

      x = 0.0;
      rhs = 0.0;
      trueX = 0.0;
      trueRhs = 0.0;

      const int dim = pmesh_->Dimension();

      VectorFunctionCoefficient fcoeff(dim, ffun);

      vel_gf = new ParGridFunction;
      vel_gf->MakeRef(fes[0], x.GetBlock(0));

      if (opt_.prob_type == PROB_TYPE::MMS)
      {
         VectorFunctionCoefficient uexcoeff(dim, vel_ex);
         vel_gf->ProjectBdrCoefficient(uexcoeff, ess_bdr_attr_);
      }
      else if (opt_.prob_type == PROB_TYPE::KOV)
      {
         VectorFunctionCoefficient kovuexcoeff(dim, kov_vel_ex);
         vel_gf->ProjectBdrCoefficient(kovuexcoeff, ess_bdr_attr_);
      }
      else if (opt_.prob_type == PROB_TYPE::LDC)
      {
         VectorFunctionCoefficient ldccoeff(dim, vel_ldc);
         vel_gf->ProjectBdrCoefficient(ldccoeff, ess_bdr_attr_);
      }
      else if (opt_.prob_type == PROB_TYPE::CYL)
      {
         VectorFunctionCoefficient cylcoeff(dim, vel_cyl);
         vel_gf->ProjectBdrCoefficient(cylcoeff, ess_bdr_attr_);
      }
      else if (opt_.prob_type == PROB_TYPE::THREEDCYL)
      {
         VectorFunctionCoefficient cylcoeff(dim, vel_threedcyl);
         vel_gf->ProjectBdrCoefficient(cylcoeff, ess_bdr_attr_);
      }

      p_gf = new ParGridFunction(fes[1]);

      // Convective nonlinear term
      // N(u,u,v) = (u \cdot \nabla u, v)
      N = new ParNonlinearForm(fes[0]);
      N->AddDomainIntegrator(new VectorConvectionNLFIntegrator);
      N->SetEssentialTrueDofs(ess_tdof_list_);

      if (opt_.prob_type == PROB_TYPE::MMS)
      {
         fform = new ParLinearForm;
         VectorDomainLFIntegrator *fvint = new VectorDomainLFIntegrator(fcoeff);
         fvint->SetIntRule(&IntRules.Get(pmesh_->GetElementBaseGeometry(0),
                                         opt_.vel_order + 3));
         fform->Update(fes[0], rhs.GetBlock(0), 0);
         fform->AddDomainIntegrator(fvint);
         fform->Assemble();
      }

      sform = new ParBilinearForm(fes[0]);
      ConstantCoefficient kin_visc(1.0 / opt_.rey);
      sform->AddDomainIntegrator(new VectorDiffusionIntegrator(kin_visc));
      sform->Assemble();
      S = new HypreParMatrix;
      sform->FormLinearSystem(ess_tdof_list_, x.GetBlock(0), rhs.GetBlock(0),
                              *S, trueX.GetBlock(0), trueRhs.GetBlock(0));

      dform = new ParMixedBilinearForm(fes[0], fes[1]);
      dform->AddDomainIntegrator(new VectorDivergenceIntegrator);
      dform->Assemble();
      D = new HypreParMatrix;
      dform->FormColLinearSystem(ess_tdof_list_, x.GetBlock(0), rhs.GetBlock(1),
                                 *D, trueX.GetBlock(0), trueRhs.GetBlock(1));

      G = D->Transpose();
      (*G) *= -1.0;

      jac = new BlockOperator(block_trueOffsets_);
      jac->SetBlock(0, 0, S);
      jac->SetBlock(0, 1, G);
      jac->SetBlock(1, 0, D);

      lin = new BlockOperator(block_trueOffsets_);
      lin->SetBlock(0, 0, S);
      lin->SetBlock(0, 1, G);
      lin->SetBlock(1, 0, D);

      // Silvester-Wathen preconditioner
      // only good for Stokes or very small Reynolds numbers

      ConstantCoefficient mpscalecoeff(1.0);
      ConstantCoefficient mpdiffscalecoeff(0.0);

      mpform = new ParBilinearForm(fes[1]);
      mpform->AddDomainIntegrator(new MassIntegrator(mpscalecoeff));
      mpform->AddDomainIntegrator(new DiffusionIntegrator(mpdiffscalecoeff));
      mpform->Assemble();
      mpform->Finalize();
      Mp = mpform->ParallelAssemble();

      invS = new HypreBoomerAMG(*S);
      static_cast<HypreBoomerAMG *>(invS)->SetPrintLevel(0);
      invS->iterative_mode = false;

      invMp = new HypreBoomerAMG(*Mp);
      static_cast<HypreBoomerAMG *>(invMp)->SetPrintLevel(0);
      invMp->iterative_mode = false;

      stokesprec = new BlockDiagonalPreconditioner(block_trueOffsets_);
      stokesprec->SetDiagonalBlock(0, invS);
      stokesprec->SetDiagonalBlock(1, invMp);

      jac_solver = new GMRESSolver(MPI_COMM_WORLD);
      jac_solver->iterative_mode = false;
      jac_solver->SetAbsTol(0.0);
      jac_solver->SetRelTol(1e-4);
      static_cast<GMRESSolver *>(jac_solver)->SetKDim(100);
      jac_solver->SetMaxIter(500);
      jac_solver->SetOperator(*jac);
      jac_solver->SetPreconditioner(*stokesprec);
      jac_solver->SetPrintLevel(2);

      newton_solver.iterative_mode = true;
      newton_solver.SetSolver(*jac_solver);
      newton_solver.SetOperator(*this);
      newton_solver.SetPrintLevel(1);
      newton_solver.SetAbsTol(0.0);
      newton_solver.SetRelTol(1e-7);
      newton_solver.SetMaxIter(15);

      // trueX.Randomize();
      // this->CheckJacobian(trueX, ess_tdof_list_);
   }

   virtual void Mult(const Vector &x, Vector &y) const
   {
      Vector tmp(block_trueOffsets_[1]);
      Vector vel_in(x.GetData(), block_trueOffsets_[1]);
      Vector vel_out(y.GetData(), block_trueOffsets_[1]);

      // Apply linear BlockOperator
      lin->Mult(x, y);

      // Apply nonlinear action to velocity
      N->Mult(vel_in, tmp);
      vel_out += tmp;
   }

   virtual Operator &GetGradient(const Vector &x) const
   {
      Vector u(x.GetData(), block_trueOffsets_[1]);

      delete NjacS;

      hypre_ParCSRMatrix *NjacS_wrap;
      hypre_ParcsrAdd(1.0, *static_cast<HypreParMatrix *>(&(N->GetGradient(u))), 1.0,
                      *S, &NjacS_wrap);
      NjacS = new HypreParMatrix(NjacS_wrap);

      HypreParMatrix *NjacS_e = NjacS->EliminateRowsCols(ess_tdof_list_);
      delete NjacS_e;

      jac->SetBlock(0, 0, NjacS);

      // invS->SetOperator(*NjacS);

      return *jac;
   }

   void Solve()
   {
      newton_solver.Mult(trueRhs, trueX);
   }

   Solver *GetJacobianSolver() const
   {
      return jac_solver;
   }

   ParGridFunction *UpdateVelocityGF() const
   {
      vel_gf->Distribute(trueX.GetBlock(0));
      return vel_gf;
   }

   ParGridFunction *UpdatePressureGF() const
   {
      p_gf->Distribute(trueX.GetBlock(1));
      return p_gf;
   }

   virtual ~NavierStokesOperator()
   {
      delete sform;
      if (opt_.prob_type == PROB_TYPE::MMS)
      {
         delete fform;
      }
      delete mpform;
      delete dform;
      delete N;
      delete S;
      delete Mp;
      delete D;
      delete G;
      delete NjacS;
      delete jac;
      delete lin;
      delete invS;
      delete invMp;
      delete stokesprec;
      delete jac_solver;
      delete vel_gf;
      delete p_gf;
   }
};

int main(int argc, char *argv[])
{
   MPI_Session mpi(argc, argv);

   int num_procs, myid;
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   int prob_type = 0;
   int print_level = 2;
   int serial_ref_levels = 0;
   int order = 2;
   double rey = 1.0;
   double ltol = 1e-8;

   OptionsParser args(argc, argv);
   args.AddOption(&order, "-o", "--order", "Polynomial order for the velocity.");
   args.AddOption(&ltol, "-ltol", "--linear_solver_tolerance",
                  "Linear solver relative tolerance.");
   args.AddOption(&print_level, "-pl", "--print-level",
                  "Solver print level.");
   args.AddOption(&serial_ref_levels, "-rs", "--serial-ref-levels",
                  "Number of serial refinement levels.");
   args.AddOption(&prob_type, "-prob", "--problem_type",
                  "Choose problem type\n\t"
                  "0 - MMS\n\t"
                  "1 - Kovasznay\n\t"
                  "2 - Lid driven cavity\n\t"
                  "3 - Flow past a cylinder\n\t"
                  "4 - 3D flow past a cylinder");
   args.AddOption(&rey, "-rey", "--reynolds", "Choose Reynolds number");
   args.Parse();
   if (!args.Good())
   {
      if (mpi.Root())
      {
         args.PrintUsage(cout);
      }
      return 1;
   }
   if (mpi.Root())
   {
      args.PrintOptions(cout);
   }

   opt_.prob_type = static_cast<PROB_TYPE>(prob_type);
   opt_.rey = rey;
   opt_.print_level = print_level;

   const char *mesh_file = nullptr;

   if (opt_.prob_type == PROB_TYPE::CYL)
   {
      mesh_file = "cyl.msh";
   }
   else if (opt_.prob_type == PROB_TYPE::THREEDCYL)
   {
      mesh_file = "3dfoc.e";
   }
   else
   {
      mesh_file = "../../data/inline-quad.mesh";
   }

   int vel_order = order;
   int pres_order = order - 1;

   Mesh *mesh = new Mesh(mesh_file);
   int dim = mesh->Dimension();

   for (int l = 0; l < serial_ref_levels; l++)
   {
      mesh->UniformRefinement();
   }

   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;

   FiniteElementCollection *vel_fec = new H1_FECollection(vel_order, dim);
   FiniteElementCollection *pres_fec = new H1_FECollection(pres_order);

   ParFiniteElementSpace *vel_fes = new ParFiniteElementSpace(pmesh, vel_fec, dim);
   ParFiniteElementSpace *pres_fes = new ParFiniteElementSpace(pmesh, pres_fec);

   Array<ParFiniteElementSpace *> fes(2);
   fes[0] = vel_fes;
   fes[1] = pres_fes;

   int fes_size0 = fes[0]->GlobalVSize();
   int fes_size1 = fes[1]->GlobalVSize();

   if (myid == 0)
   {
      cout << "Velocity #DOFs: " << fes_size0 << endl;
      cout << "Pressure #DOFs: " << fes_size1 << endl;
   }

   NavierStokesOperator nso(fes);

   nso.Solve();

   ParGridFunction *vel_gf = nso.UpdateVelocityGF();
   ParGridFunction *p_gf = nso.UpdatePressureGF();

   if (opt_.prob_type == PROB_TYPE::MMS ||
       opt_.prob_type == PROB_TYPE::KOV)
   {
      int order_quad = max(2, 2 * order + 1);
      const IntegrationRule *irs[Geometry::NumGeom];
      for (int i = 0; i < Geometry::NumGeom; ++i)
      {
         irs[i] = &(IntRules.Get(i, order_quad));
      }

      VectorFunctionCoefficient *uexcoeff = nullptr;
      FunctionCoefficient *pexcoeff = nullptr;

      if (opt_.prob_type == PROB_TYPE::MMS)
      {
         uexcoeff = new VectorFunctionCoefficient(dim, vel_ex);
         pexcoeff = new FunctionCoefficient(p_ex);
      }
      else if (opt_.prob_type == PROB_TYPE::KOV)
      {
         uexcoeff = new VectorFunctionCoefficient(dim, kov_vel_ex);
         pexcoeff = new FunctionCoefficient(kov_p_ex);
      }

      double err_u = vel_gf->ComputeL2Error(*uexcoeff, irs);
      double norm_u = ComputeGlobalLpNorm(2, *uexcoeff, *pmesh, irs);

      double err_p = p_gf->ComputeL2Error(*pexcoeff, irs);
      double norm_p = ComputeGlobalLpNorm(2, *pexcoeff, *pmesh, irs);

      if (myid == 0)
      {
         cout << "|| u_h - u_ex || = " << err_u << "\n";
         cout << "|| u_h - u_ex || / || u_ex || = " << err_u / norm_u << "\n";
         cout << "|| p_h - p_ex || = " << err_p << "\n";
         cout << "|| p_h - p_ex || / || p_ex || = " << err_p / norm_p << "\n";
      }

      delete uexcoeff;
      delete pexcoeff;
   }

   char vishost[] = "localhost";
   int visport = 19916;
   socketstream u_sock(vishost, visport);
   u_sock << "parallel " << num_procs << " " << myid << "\n";
   u_sock.precision(8);
   u_sock << "solution\n"
          << *pmesh << *vel_gf << "window_title 'velocity'"
          << "keys Rjlc\n"
          << endl;

   socketstream p_sock(vishost, visport);
   p_sock << "parallel " << num_procs << " " << myid << "\n";
   p_sock.precision(8);
   p_sock << "solution\n"
          << *pmesh << *p_gf << "window_title 'pressure'"
          << "keys Rjlc\n"
          << endl;

   delete vel_fec;
   delete pres_fec;
   delete vel_fes;
   delete pres_fes;
   delete pmesh;

   return 0;
}
