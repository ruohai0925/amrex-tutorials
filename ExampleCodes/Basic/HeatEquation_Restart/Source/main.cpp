#include <AMReX_PlotFileUtil.H>
#include <AMReX_ParmParse.H>

#include "myfunc.H"

using namespace amrex;

int main (int argc, char* argv[])
{
    amrex::Initialize(argc,argv);

    main_main();

    amrex::Finalize();
    return 0;
}

void main_main ()
{

    // **********************************
    // SIMULATION PARAMETERS

    // number of cells on each side of the domain
    int n_cell;

    // size of each box (or grid)
    int max_grid_size;

    // total steps in simulation
    int nsteps;

    // time step
    Real dt;

    // how often to write a plotfile
    int plot_int;

    // how often to write a checkpoint file
    int chk_int;

    // restart = -1 --> start new simulation
    // restart >  0 --> restart from step=restart
    int restart;

    // inputs parameters
    {
        // ParmParse is way of reading inputs from the inputs file
        // pp.get means we require the inputs file to have it
        // pp.query means we optionally need the inputs file to have it - but we must supply a default here
        ParmParse pp;

        // We need to get n_cell from the inputs file - this is the number of cells on each side of
        //   a square (or cubic) domain.
        pp.get("n_cell",n_cell);

        // The domain is broken into boxes of size max_grid_size
        pp.get("max_grid_size",max_grid_size);

        // Default nsteps to 10, allow us to set it to something else in the inputs file
        nsteps = 10;
        pp.query("nsteps",nsteps);

        // time step
        pp.get("dt",dt);

        // Default plot_int to -1, allow us to set it to something else in the inputs file
        //  If plot_int < 0 then no plot files will be written
        plot_int = -1;
        pp.query("plot_int",plot_int);

        // Default chk_int to -1, allow us to set it to something else in the inputs file
        //  If chk_int < 0 then no chk files will be written
        chk_int = -1;
        pp.query("chk_int",chk_int);

        // restart = -1 --> start new simulation
        // restart >  0 --> restart from step=restart
        restart = -1;
        pp.query("restart",restart);

    }

    // **********************************
    // SIMULATION SETUP

    // make Geometry object
    // geom contains information such as the physical domain size,
    //               number of points in the domain, and periodicity
    Geometry geom;

    // AMREX_D_DECL means "do the first X of these, where X is the dimensionality of the simulation"
    IntVect dom_lo(AMREX_D_DECL(       0,        0,        0));
    IntVect dom_hi(AMREX_D_DECL(n_cell-1, n_cell-1, n_cell-1));

    // Make a single box that is the entire domain
    Box domain(dom_lo, dom_hi);

    // This defines the physical box, [0,1] in each direction.
    RealBox real_box({AMREX_D_DECL( 0., 0., 0.)},
                     {AMREX_D_DECL( 1., 1., 1.)});

    // periodic in all direction
    Array<int,AMREX_SPACEDIM> is_periodic{AMREX_D_DECL(1,1,1)};

    // This defines a Geometry object
    geom.define(domain, real_box, CoordSys::cartesian, is_periodic);

    // extract dx from the geometry object
    GpuArray<Real,AMREX_SPACEDIM> dx = geom.CellSizeArray();

    // create a BoxArray and DistributionMapping
    // ba will contain a list of boxes that cover the domain
    // if restarting, BoxArray is read in from the checkpoint directory
    BoxArray ba;
    DistributionMapping dm;

    // simulation time
    Real time;

    // start step - will be 1 unless restarting
    int start_step;

    MultiFab phi_old;
    MultiFab phi_new;

    // Nghost = number of ghost cells for each array
    int Nghost = 1;

    // Ncomp = number of components for each array
    int Ncomp = 1;

    // start a new simulation
    if (restart <= 0) {

        // Initialize the boxarray "ba" from the single box "domain"
        ba.define(domain);

        // Break up boxarray "ba" into chunks no larger than "max_grid_size" along a direction
        ba.maxSize(max_grid_size);

        // How Boxes are distrubuted among MPI processes
        dm.define(ba);

        // we allocate two phi multifabs; one will store the old state, the other the new.
        phi_old.define(ba, dm, Ncomp, Nghost);
        phi_new.define(ba, dm, Ncomp, Nghost);

        // time = starting time in the simulation
        time = 0.0;

        start_step = 1;

        // **********************************
        // INITIALIZE DATA

        // loop over boxes
        for (MFIter mfi(phi_old); mfi.isValid(); ++mfi)
        {
            const Box& bx = mfi.validbox();

            const Array4<Real>& phiOld = phi_old.array(mfi);

            // set phi = 1 + e^(-(r-0.5)^2)
            amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE(int i, int j, int k)
            {
                Real x = (i+0.5) * dx[0];
                Real y = (j+0.5) * dx[1];
#if (AMREX_SPACEDIM == 2)
                Real rsquared = ((x-0.5)*(x-0.5)+(y-0.5)*(y-0.5))/0.01;
#elif (AMREX_SPACEDIM == 3)
                Real z= (k+0.5) * dx[2];
                Real rsquared = ((x-0.5)*(x-0.5)+(y-0.5)*(y-0.5)+(z-0.5)*(z-0.5))/0.01;
#endif
                phiOld(i,j,k) = 1. + std::exp(-rsquared);
            });
        }

        // Write a plotfile of the initial data if plot_int > 0
        if (plot_int > 0)
        {
            int step = 0;
            const std::string& pltfile = amrex::Concatenate("plt",step,5);
            WriteSingleLevelPlotfile(pltfile, phi_old, {"phi"}, geom, time, 0);
        }

    } else {

        // restart a simulation
        // read in the time and BoxArray, then create a DistributionMapping
        // Define phi_old and fill it with data from the checkpoint file
        ReadCheckpoint(restart, time, phi_old, ba, dm);

        // set start_step to restart+1
        start_step = restart + 1;

        phi_new.define(ba, dm, Ncomp, Nghost);
    }

    for (int step = start_step; step <= nsteps; ++step)
    {
        // fill periodic ghost cells
        phi_old.FillBoundary(geom.periodicity());

        // new_phi = old_phi + dt * Laplacian(old_phi)
        // loop over boxes
        for ( MFIter mfi(phi_old); mfi.isValid(); ++mfi )
        {
            const Box& bx = mfi.validbox();

            const Array4<Real>& phiOld = phi_old.array(mfi);
            const Array4<Real>& phiNew = phi_new.array(mfi);

            // advance the data by dt
            amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k)
            {
                phiNew(i,j,k) = phiOld(i,j,k) + dt *
                    ( (phiOld(i+1,j,k) - 2.*phiOld(i,j,k) + phiOld(i-1,j,k)) / (dx[0]*dx[0])
                     +(phiOld(i,j+1,k) - 2.*phiOld(i,j,k) + phiOld(i,j-1,k)) / (dx[1]*dx[1])
#if (AMREX_SPACEDIM == 3)
                     +(phiOld(i,j,k+1) - 2.*phiOld(i,j,k) + phiOld(i,j,k-1)) / (dx[2]*dx[2])
#endif
                        );
            });
        }

        // update time
        time = time + dt;

        // copy new solution into old solution
        MultiFab::Copy(phi_old, phi_new, 0, 0, 1, 0);

        // Tell the I/O Processor to write out which step we're doing
        amrex::Print() << "Advanced step " << step << "\n";

        // Write a plotfile of the current data (plot_int was defined in the inputs file)
        if (plot_int > 0 && step%plot_int == 0)
        {
            const std::string& pltfile = amrex::Concatenate("plt",step,5);
            WriteSingleLevelPlotfile(pltfile, phi_new, {"phi"}, geom, time, step);
        }

        // Write a checkpoint file of the current data (chk_int was defined in the inputs file)
        if (chk_int > 0 && step%chk_int == 0)
        {
            // create a checkpoint directory
            // write out time and BoxArray to a Header file
            // write out multifab data
            WriteCheckpoint(step,time,phi_new);
        }
    }
}
