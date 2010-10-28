//////////////////////////////////////////////////////////////////
// (c) Copyright 2005- by Jeongnim Kim
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//   Jeongnim Kim
//   National Center for Supercomputing Applications &
//   Materials Computation Center
//   University of Illinois, Urbana-Champaign
//   Urbana, IL 61801
//   e-mail: jnkim@ncsa.uiuc.edu
//   Tel:    217-244-6319 (NCSA) 217-333-3324 (MCC)
//
// Supported by
//   National Center for Supercomputing Applications, UIUC
//   Materials Computation Center, UIUC
//////////////////////////////////////////////////////////////////
// -*- C++ -*-
#include "QMCDrivers/QMCLinearOptimize.h"
#include "Particle/HDFWalkerIO.h"
#include "Particle/DistanceTable.h"
#include "OhmmsData/AttributeSet.h"
#include "Message/CommOperators.h"
#if defined(ENABLE_OPENMP)
#include "QMCDrivers/VMC/VMCSingleOMP.h"
#include "QMCDrivers/QMCCostFunctionOMP.h"
#endif
#include "QMCDrivers/VMC/VMCSingle.h"
#include "QMCDrivers/QMCCostFunctionSingle.h"
#include "QMCApp/HamiltonianPool.h"
#include "Numerics/Blasf.h"
#include <cassert>
#if defined(QMC_CUDA)
#include "QMCDrivers/VMC/VMC_CUDA.h"
#include "QMCDrivers/QMCCostFunctionCUDA.h"
#endif
#include "Numerics/LinearFit.h"
#include <iostream>
#include <fstream>

/*#include "Message/Communicate.h"*/

namespace qmcplusplus
{

QMCLinearOptimize::QMCLinearOptimize(MCWalkerConfiguration& w,
                                     TrialWaveFunction& psi, QMCHamiltonian& h, HamiltonianPool& hpool): QMCDriver(w,psi,h),
        PartID(0), NumParts(1), WarmupBlocks(10), SkipSampleGeneration("no"), hamPool(hpool),
        optTarget(0), vmcEngine(0), Max_iterations(1), wfNode(NULL), optNode(NULL), allowedCostDifference(1.0e-6),
        exp0(-16), exp1(0),  nstabilizers(10), stabilizerScale(0.5), bigChange(1), eigCG(1), TotalCGSteps(2), w_beta(0.0),
        MinMethod("quartic"), GEVtype("mixed"), StabilizerMethod("best"), GEVSplit("no")
{
    //set the optimization flag
    QMCDriverMode.set(QMC_OPTIMIZE,1);
    //read to use vmc output (just in case)
    RootName = "pot";
    QMCType ="QMCLinearOptimize";
    optmethod = "Linear";
    m_param.add(WarmupBlocks,"warmupBlocks","int");
    m_param.add(SkipSampleGeneration,"skipVMC","string");
    m_param.add(Max_iterations,"max_its","int");
    m_param.add(nstabilizers,"nstabilizers","int");
    m_param.add(stabilizerScale,"stabilizerscale","double");
    m_param.add(allowedCostDifference,"alloweddifference","double");
    m_param.add(bigChange,"bigchange","double");
    m_param.add(eigCG,"eigcg","int");
    m_param.add(TotalCGSteps,"cgsteps","int");
    m_param.add(w_beta,"beta","double");
    quadstep=-1.0;
    m_param.add(quadstep,"stepsize","double");
    m_param.add(exp0,"exp0","double");
    m_param.add(exp1,"exp1","double");
    m_param.add(MinMethod,"MinMethod","string");
    m_param.add(GEVtype,"GEVMethod","string");
    m_param.add(GEVSplit,"GEVSplit","string");
    m_param.add(StabilizerMethod,"StabilizerMethod","string");
    m_param.add(LambdaMax,"LambdaMax","double");
    //Set parameters for line minimization:
    this->add_timers(myTimers);
}

/** Clean up the vector */
QMCLinearOptimize::~QMCLinearOptimize()
{
    delete vmcEngine;
    delete optTarget;
}

void QMCLinearOptimize::add_timers(vector<NewTimer*>& timers)
{
    timers.push_back(new NewTimer("QMCLinearOptimize::GenerateSamples"));
    timers.push_back(new NewTimer("QMCLinearOptimize::Initialize"));
    timers.push_back(new NewTimer("QMCLinearOptimize::Eigenvalue"));
    timers.push_back(new NewTimer("QMCLinearOptimize::Line_Minimization"));
    timers.push_back(new NewTimer("QMCLinearOptimize::GradCost"));
    for (int i=0; i<timers.size(); ++i) TimerManager.addTimer(timers[i]);
}

QMCLinearOptimize::RealType QMCLinearOptimize::Func(RealType dl)
{
    for (int i=0; i<optparm.size(); i++) optTarget->Params(i) = optparm[i] + dl*optdir[i];
    QMCLinearOptimize::RealType c = optTarget->Cost(false);
    //only allow this to go false if it was true. If false, stay false
    if (validFuncVal) validFuncVal = optTarget->IsValid;
    return c;
}

/** Add configuration files for the optimization
* @param a root of a hdf5 configuration file
*/
void QMCLinearOptimize::addConfiguration(const string& a)
{
    if (a.size()) ConfigFile.push_back(a);
}

void QMCLinearOptimize::start()
{
    optTarget->initCommunicator(myComm);
    //close files automatically generated by QMCDriver
    //branchEngine->finalize();


    //generate samples
    myTimers[0]->start();
    generateSamples();
    myTimers[0]->stop();

    app_log() << "<opt stage=\"setup\">" << endl;
    app_log() << "  <log>"<<endl;

    //reset the rootname
    optTarget->setRootName(RootName);
    optTarget->setWaveFunctionNode(wfNode);

    app_log() << "   Reading configurations from h5FileRoot " << endl;
    //get configuration from the previous run
    Timer t1;


    myTimers[1]->start();
    optTarget->getConfigurations(h5FileRoot);
    optTarget->checkConfigurations();
    myTimers[1]->stop();

    app_log() << "  Execution time = " << t1.elapsed() << endl;
    app_log() << "  </log>"<<endl;
    app_log() << "</opt>" << endl;

    app_log() << "<opt stage=\"main\" walkers=\""<< optTarget->getNumSamples() << "\">" << endl;
    app_log() << "  <log>" << endl;
//       optTarget->setTargetEnergy(branchEngine->getEref());
    app_log()<<"  GEV method "<<GEVtype<<endl;
    app_log()<<"  Split EV   "<<GEVSplit<<endl;
    app_log()<<"  Line Minimization method "<<MinMethod<<endl;

    t1.restart();
}

bool QMCLinearOptimize::ValidCostFunction(bool valid)
{
    if (!valid) app_log()<<" Cost Function is Invalid. If this frequently, try reducing the step size of the line minimization or reduce the number of cycles. " <<endl;
    return valid;
}

bool QMCLinearOptimize::run()
{
    start();
    bool Valid(true);
    int Total_iterations(0);
    savedQuadstep=quadstep;

//size of matrix
    numParams = optTarget->NumParams();
    N = numParams + 1;

//  solve CSFs and other parameters separately then rescale elements accordingly
    int first,last;
    getNonLinearRange(first,last);
//  There is only one type of parameter and all are non-linear, don't split it up
    if (last-first==numParams) GEVSplit=="no";

//     initialize our parameters
    vector<RealType> currentParameterDirections(N,0);
    vector<RealType> currentParameters(numParams,0);
    optdir.resize(numParams,0);
    for (int i=0; i<numParams; i++) currentParameters[i] = optTarget->Params(i);

    Matrix<RealType> Ham(N,N);
    Matrix<RealType> Ham2(N,N);
    Matrix<RealType> Var(N,N);
    Matrix<RealType> S(N,N);
    vector<RealType> BestDirection(N,0);
    vector<RealType> bestParameters(currentParameters);
    vector<RealType> GEVSplitParameters(numParams,0);


    while (Total_iterations < Max_iterations)
    {
        Total_iterations+=1;
        app_log()<<"Iteration: "<<Total_iterations<<"/"<<Max_iterations<<endl;

// mmorales
        if (!ValidCostFunction(Valid)) continue;

//this is the small amount added to the diagonal to stabilize the eigenvalue equation. 10^stabilityBase
        RealType stabilityBase(exp0);
        //This is the amount we add to the linear parameters
        RealType linearStabilityBase(exp1);

        vector<vector<RealType> > LastDirections;
        RealType deltaPrms(-1.0);
        for (int tries=0; tries<TotalCGSteps; tries++)
        {
            bool acceptedOneMove(false);
            int tooManyTries(40);

            Matrix<RealType> Left(N,N);
            Matrix<RealType> Right(N,N);

            vector<std::pair<RealType,RealType> > mappedStabilizers;
            if (nstabilizers<5)
            {
                if (StabilizerMethod=="fit") app_log()<<" Need 5 stabilizers minimum for the fit"<<endl;
                StabilizerMethod="best";
            }

            for (int i=0; i<numParams; i++) optTarget->Params(i) = currentParameters[i];

            myTimers[4]->start();
            RealType lastCost(optTarget->Cost(true));
            myTimers[4]->start();

            RealType newCost(lastCost);
            optTarget->fillOverlapHamiltonianMatrices(Ham2, Ham, Var, S);

// mmorales
            Valid=optTarget->IsValid;
            if (!ValidCostFunction(Valid)) continue;

            if (GEVtype=="H2")
            {
                Left=Ham;
                RealType H2rescale=1.0/Ham2(0,0);
                Right=(1-w_beta)*S + w_beta*H2rescale*Ham2;
            }
            else
            {
                Right=S;
                Left=(1.0-w_beta)*Ham + w_beta*Var;
            }

            //Find largest off-diagonal element compared to diagonal element.
            //This gives us an idea how well conditioned it is and can be used to stabilize.
            RealType od_largest(0);
            for (int i=0; i<N; i++) for (int j=0; j<N; j++)
              od_largest=std::max( std::max(od_largest,std::abs(Left(i,j))-std::abs(Left(i,i))), std::abs(Left(i,j))-std::abs(Left(j,j)));
            od_largest = std::log(od_largest);

            RealType safe = Left(0,0);
            for (int stability=0; stability<nstabilizers; stability++)
            {
                Matrix<RealType> LeftT(N,N), RightT(N,N);
                for (int i=0; i<N; i++)
                    for (int j=0; j<N; j++)
                    {
                        LeftT(i,j)= Left(j,i);
                        RightT(i,j)= Right(j,i);
                    }


                RealType XS(0);
                if ((StabilizerMethod=="fit")&&(stability==nstabilizers-1))
                {
                    //Quartic fit the stabilizers we have tried and try to choose the best we can
                    int nms=mappedStabilizers.size();
                    
                    
                    
                    vector<RealType>  Y(nms), Coefs(5);
                    Matrix<RealType> X(nms,5);
                    for (int i=0; i<nms; i++) X(i,0)=1.0;
                    for (int i=0; i<nms; i++) X(i,1)=mappedStabilizers[i].second;
                    for (int i=0; i<nms; i++) X(i,2)=X(i,1)*X(i,1);
                    for (int i=0; i<nms; i++) X(i,3)=X(i,2)*X(i,1);
                    for (int i=0; i<nms; i++) X(i,4)=X(i,3)*X(i,1);
                    for (int i=0; i<nms; i++) Y[i]=mappedStabilizers[i].first;
                    LinearFit(Y,X,Coefs);
                    //lowest we will allow
                    RealType lowestExp = std::min(exp0 - 0.25*std::abs(exp0), exp0-2.0*stabilizerScale);

                    RealType dltaBest=std::max(lowestExp , QuarticMinimum(Coefs));
                    XS = dltaBest;
                    stability=nstabilizers;
                }

                RealType lowestEV(0);
                if ((GEVSplit=="rescale")||(GEVSplit=="freeze"))
                {
                  //These are experimental and aren't very good.
                    //dummy bool
                    bool CSF_lower(true);
                    lowestEV = getSplitEigenvectors(first,last,LeftT,RightT,currentParameterDirections,GEVSplitParameters,GEVSplit,CSF_lower);
                }
                else if (GEVSplit=="stability") //This seems to work pretty well.
                {
                    if (XS==0)
                    {
                        od_largest=std::max(od_largest,stabilityBase+nstabilizers*stabilizerScale);
                        RealType spart = (1.0*stability)/nstabilizers;
                        XS     = std::exp((1.0-spart)*stabilityBase + spart*od_largest);
                        for (int i=first; i<last; i++) LeftT(i+1,i+1) += XS;
                        
                        RealType XS_lin = std::exp(linearStabilityBase + (1.0-spart)*stabilityBase + spart*od_largest);
                        if (first==0) for (int i=last; i<N; i++) LeftT(i+1,i+1) += XS_lin;
                        else for (int i=0; i<first; i++) LeftT(i+1,i+1) += XS_lin;
                    }
                    else //else XS is from the quartic fit
                    {
                      //Not sure how to control for the quartic fit and the two different stabilizers. This seems ok.
                      //Better algorithm exists?
                        for (int i=first; i<last; i++) LeftT(i+1,i+1) += std::exp(XS);
                      
                        RealType XS_lin = std::exp(linearStabilityBase+XS);
                        if (first==0) for (int i=last; i<N; i++) LeftT(i+1,i+1) += XS_lin;
                        for (int i=0; i<first; i++) LeftT(i+1,i+1) += XS_lin;
                    }

                    if (stability==0)
                    {
                        //  Only need to do this the first time we step into the routine
                        bool CSF_lower(true);
                        lowestEV=getSplitEigenvectors(first,last,LeftT,RightT,currentParameterDirections,GEVSplitParameters,GEVSplit,CSF_lower);
                        if (tooLow(safe,lowestEV))
                        {
                            if (CSF_lower)
                            {
                                linearStabilityBase+=stabilizerScale;
                                app_log()<<"Probably will not converge: CSF Eigenvalue="<<lowestEV<<" LeftT(0,0)="<<safe<<endl;
                            }
                            else
                            {
                                linearStabilityBase-=stabilizerScale;
                                stabilityBase+=stabilizerScale;
                                app_log()<<"Probably will not converge: Jas Eigenvalue="<<lowestEV<<" LeftT(0,0)="<<safe<<endl;
                            }
                            //maintain same number of "good" stability tries
                            stability-=1;
                            continue;
                        }
                    }
                    
                    myTimers[2]->start();
                    lowestEV =getLowestEigenvector(LeftT,RightT,currentParameterDirections);
                    myTimers[2]->stop();
                }
                else
                {
                    if (XS==0)
                    {
                      od_largest=std::max(od_largest,stabilityBase+nstabilizers*stabilizerScale);
                      RealType spart = (1.0*stability)/nstabilizers;
                      XS     = std::exp((1.0-spart)*stabilityBase + spart*od_largest);
                      for (int i=1; i<N; i++) LeftT(i,i) += XS;
                    }
                    else
                    {
                      //else XS is from the quartic fit
                      for (int i=1; i<N; i++) LeftT(i,i) += std::exp(XS);
                    }
                    
                    myTimers[2]->start();
                    lowestEV =getLowestEigenvector(LeftT,RightT,currentParameterDirections);
                    myTimers[2]->stop();
                }

                if (tooLow(safe,lowestEV))
                {
                    app_log()<<"Probably will not converge: Eigenvalue="<<lowestEV<<" LeftT(0,0)="<<safe<<endl;
                    //try a larger stability base and repeat
                    stabilityBase+=stabilizerScale;
                    //maintain same number of "good" stability tries
                    stability-=1;
                    continue;
                }

                if (MinMethod=="rescale")
                {
//                   method from umrigar
                    myTimers[3]->start();
                    Lambda = getNonLinearRescale(currentParameterDirections,S);
                    myTimers[3]->stop();

                    RealType bigVec(0);
                    for (int i=0; i<numParams; i++) bigVec = std::max(bigVec,std::abs(currentParameterDirections[i+1]));
                    if (Lambda*bigVec>bigChange)
                    {
                        app_log()<<"  Failed Step. Largest parameter change: "<<Lambda*bigVec<<endl;
                        tooManyTries--;
                        if (tooManyTries>0)
                        {
                            stabilityBase+=stabilizerScale;
                            stability-=1;
                            app_log()<<" Re-run with larger stabilityBase"<<endl;
                            continue;
                        }
                    }
                    else
                        for (int i=0; i<numParams; i++) optTarget->Params(i) = currentParameters[i] + Lambda*currentParameterDirections[i+1];
                }
                else
                {
                    //eigenCG part
                    for (int ldi=0; ldi < std::min(eigCG,int(LastDirections.size())); ldi++)
                    {
                        RealType nrmold(0), ovlpold(0);
                        for (int i=1; i<N; i++) nrmold += LastDirections[ldi][i]*LastDirections[ldi][i];
                        for (int i=1; i<N; i++) ovlpold += LastDirections[ldi][i]*currentParameterDirections[i];
                        ovlpold*=1.0/nrmold;
                        for (int i=1; i<N; i++) currentParameterDirections[i] -= ovlpold * LastDirections[ldi][i];
                    }

                    optparm.resize(numParams);
                    for (int i=0; i<numParams; i++) optparm[i] = currentParameters[i] + GEVSplitParameters[i];
                    for (int i=0; i<numParams; i++) optdir[i] = currentParameterDirections[i+1];
                    RealType bigVec(0);
                    for (int i=0; i<numParams; i++) bigVec = std::max(bigVec,std::abs(optdir[i]));

                    TOL = allowedCostDifference/bigVec;

                    largeQuarticStep=bigChange/bigVec;
                    if (savedQuadstep>0) quadstep=savedQuadstep/bigVec;
                    else if (deltaPrms>0) quadstep=deltaPrms/bigVec;
                    else quadstep = getNonLinearRescale(currentParameterDirections,S);
                    //use the rescaling from umrigar everytime for the quartic guess
                    if (MinMethod=="quartic_u") quadstep = getNonLinearRescale(currentParameterDirections,S);

                    myTimers[3]->start();
                    if ((MinMethod=="quartic")||(MinMethod=="quartic_u"))  Valid=lineoptimization();
                    else Valid=lineoptimization2();
                    myTimers[3]->stop();

                    RealType biggestParameterChange = bigVec*std::abs(Lambda);
                    if ( biggestParameterChange>bigChange )
                    {
                        app_log()<<"  Failed Step. Largest parameter change:"<<biggestParameterChange<<endl;
//                     optTarget->printEstimates();
                        tooManyTries--;
                        if (tooManyTries>0)
                        {
                            stabilityBase+=stabilizerScale;
                            stability-=1;
                            app_log()<<" Re-run with larger stabilityBase"<<endl;
                            continue;
                        }
                    }
                    else for (int i=0; i<numParams; i++) optTarget->Params(i) = optparm[i] + Lambda * optdir[i];
                    Lambda = biggestParameterChange;
                }
                //get cost at new minimum
                newCost = optTarget->Cost(false);

                // mmorales
                Valid=optTarget->IsValid;
                if (!ValidCostFunction(Valid)) continue;

                if (StabilizerMethod=="fit")
                {
                    std::pair<RealType,RealType> ms;
                    ms.first=newCost;
//                     the log fit seems to work best
                    ms.second=std::log10(XS);
                    mappedStabilizers.push_back(ms);
                }


                app_log()<<" OldCost: "<<lastCost<<" NewCost: "<<newCost<<endl;
                optTarget->printEstimates();
//                 quit if newcost is greater than lastcost. E(Xs) looks quadratic (between steepest descent and parabolic)

                if ((newCost < lastCost)&&(newCost==newCost))
                {
                    //Move was acceptable
                    for (int i=0; i<numParams; i++) bestParameters[i] = optTarget->Params(i);
                    lastCost=newCost;
                    BestDirection=currentParameterDirections;
                    acceptedOneMove=true;

                    deltaPrms= Lambda;
                }
                else if (newCost>lastCost+1.0e-4)
                {
                    int neededForGoodQuarticFit=5;//really one more so if 5, then 6 values kept. 4 is minimum.
                    if ((StabilizerMethod=="fit")&&(stability < neededForGoodQuarticFit))
                    {
                        app_log()<<"Small change, but need "<< neededForGoodQuarticFit+1 <<" values for a good quartic stability fit."<<endl;
                    }
                    else if ((StabilizerMethod=="fit")&&(stability >= neededForGoodQuarticFit))
                    {
                        stability = max(nstabilizers-2,stability);
                        if (stability==nstabilizers-2) app_log()<<"Small change, moving on to quartic fit."<<endl;
                        else app_log()<<"Moving on to next eigCG or iteration."<<endl;
                    }
                    else
                    {
                        stability = nstabilizers;
                        app_log()<<"Small change, moving on to next eigCG or iteration."<<endl;
                    }
                }
            }

            if (acceptedOneMove)
            {
                for (int i=0; i<numParams; i++) optTarget->Params(i) = bestParameters[i];
                currentParameters=bestParameters;
                LastDirections.push_back(BestDirection);
//             app_log()<< " Wave Function Parameters updated."<<endl;
//             optTarget->reportParameters();
            }
            else
            {
                for (int i=0; i<numParams; i++) optTarget->Params(i) = currentParameters[i];
                tries=TotalCGSteps;
            }

        }
    }
    finish();
    return (optTarget->getReportCounter() > 0);
}

void QMCLinearOptimize::finish()
{
    MyCounter++;
    app_log() << "  Execution time = " << t1.elapsed() << endl;
    TimerManager.print(myComm);
    TimerManager.reset();
    app_log() << "  </log>" << endl;
    optTarget->reportParameters();
    app_log() << "</opt>" << endl;
    app_log() << "</optimization-report>" << endl;
}

void QMCLinearOptimize::generateSamples()
{

    app_log() << "<optimization-report>" << endl;
    //if(WarmupBlocks)
    //{
    //  app_log() << "<vmc stage=\"warm-up\" blocks=\"" << WarmupBlocks << "\">" << endl;
    //  //turn off QMC_OPTIMIZE
    //  vmcEngine->setValue("blocks",WarmupBlocks);
    //  vmcEngine->QMCDriverMode.set(QMC_WARMUP,1);
    //  vmcEngine->run();
    //  vmcEngine->setValue("blocks",nBlocks);
    //  app_log() << "  Execution time = " << t1.elapsed() << endl;
    //  app_log() << "</vmc>" << endl;
    //}

    if (W.getActiveWalkers()>NumOfVMCWalkers)
    {
        W.destroyWalkers(W.getActiveWalkers()-NumOfVMCWalkers);
        app_log() << "  QMCLinearOptimize::generateSamples removed walkers." << endl;
        app_log() << "  Number of Walkers per node " << W.getActiveWalkers() << endl;
    }

    vmcEngine->QMCDriverMode.set(QMC_OPTIMIZE,1);
    vmcEngine->QMCDriverMode.set(QMC_WARMUP,0);

    //vmcEngine->setValue("recordWalkers",1);//set record
    vmcEngine->setValue("current",0);//reset CurrentStep
    app_log() << "<vmc stage=\"main\" blocks=\"" << nBlocks << "\">" << endl;
    t1.restart();
    //     W.reset();
    branchEngine->flush(0);
    branchEngine->reset();
    vmcEngine->run();
    app_log() << "  Execution time = " << t1.elapsed() << endl;
    app_log() << "</vmc>" << endl;

    //write parameter history and energies to the parameter file in the trial wave function through opttarget
    RealType e,w,var;
    vmcEngine->Estimators->getEnergyAndWeight(e,w,var);
    optTarget->recordParametersToPsi(e,var);

    //branchEngine->Eref=vmcEngine->getBranchEngine()->Eref;
//         branchEngine->setTrialEnergy(vmcEngine->getBranchEngine()->getEref());
    //set the h5File to the current RootName
    h5FileRoot=RootName;
}

QMCLinearOptimize::RealType QMCLinearOptimize::getLowestEigenvector(Matrix<RealType>& A, Matrix<RealType>& B, vector<RealType>& ev)
{
    int Nl(ev.size());
    //Tested the single eigenvalue speed and It was no faster.
    //segfault issues with single eigenvalue problem for some machines
    bool singleEV(false);
    if (singleEV)
    {
        Matrix<double> TAU(Nl,Nl);
        int INFO;
        int LWORK(-1);
        vector<RealType> WORK(1);
        //optimal work size
        dgeqrf( &Nl, &Nl, B.data(), &Nl, TAU.data(), &WORK[0], &LWORK, &INFO);
        LWORK=int(WORK[0]);
        WORK.resize(LWORK);
        //QR factorization of S, or H2 matrix. to be applied to H before solve.
        dgeqrf( &Nl, &Nl, B.data(), &Nl, TAU.data(), &WORK[0], &LWORK, &INFO);

        char SIDE('L');
        char TRANS('T');
        LWORK=-1;
        //optimal work size
        dormqr(&SIDE, &TRANS, &Nl, &Nl, &Nl, B.data(), &Nl, TAU.data(), A.data(), &Nl, &WORK[0], &LWORK, &INFO);
        LWORK=int(WORK[0]);
        WORK.resize(LWORK);
        //Apply Q^T to H
        dormqr(&SIDE, &TRANS, &Nl, &Nl, &Nl, B.data(), &Nl, TAU.data(), A.data(), &Nl, &WORK[0], &LWORK, &INFO);

        //now we have a pair (A,B)=(Q^T*H,Q^T*S) where B is upper triangular and A is general matrix.
        //reduce the matrix pair to generalized upper Hesenberg form
        char COMPQ('N'), COMPZ('I');
        int ILO(1);
        int LDQ(Nl);
        Matrix<double> Z(Nl,Nl), Q(Nl,LDQ); //starts as unit matrix
        for (int zi=0; zi<Nl; zi++) Z(zi,zi)=1;
        dgghrd(&COMPQ, &COMPZ, &Nl, &ILO, &Nl, A.data(), &Nl, B.data(), &Nl, Q.data(), &LDQ, Z.data(), &Nl, &INFO);

        //Take the pair and reduce to shur form and get eigenvalues
        vector<RealType> alphar(Nl),alphai(Nl),beta(Nl);
        char JOB('S');
        COMPQ='N';
        COMPZ='V';
        LWORK=-1;
        //get optimal work size
        dhgeqz(&JOB, &COMPQ, &COMPZ, &Nl, &ILO, &Nl, A.data(), &Nl, B.data(), &Nl, &alphar[0], &alphai[0], &beta[0], Q.data(), &LDQ, Z.data(), &Nl, &WORK[0], &LWORK, &INFO);
        LWORK=int(WORK[0]);
        WORK.resize(LWORK);
        dhgeqz(&JOB, &COMPQ, &COMPZ, &Nl, &ILO, &Nl, A.data(), &Nl, B.data(), &Nl, &alphar[0], &alphai[0], &beta[0], Q.data(), &LDQ, Z.data(), &Nl, &WORK[0], &LWORK, &INFO);
        //find the best eigenvalue
        vector<std::pair<RealType,int> > mappedEigenvalues(Nl);
        for (int i=0; i<Nl; i++)
        {
            RealType evi(alphar[i]/beta[i]);
            if (abs(evi)<1e10)
            {
                mappedEigenvalues[i].first=evi;
                mappedEigenvalues[i].second=i;
            }
            else
            {
                mappedEigenvalues[i].first=1e100;
                mappedEigenvalues[i].second=i;
            }
        }
        std::sort(mappedEigenvalues.begin(),mappedEigenvalues.end());
        int BestEV(mappedEigenvalues[0].second);

//                   now we rearrange the  the matrices
        if (BestEV!=0)
        {
            bool WANTQ(false);
            bool WANTZ(true);
            int ILST(1);
            int IFST(BestEV+1);
            LWORK=-1;

            dtgexc(&WANTQ, &WANTZ, &Nl, A.data(), &Nl, B.data(), &Nl, Q.data(), &Nl, Z.data(), &Nl, &IFST, &ILST, &WORK[0], &LWORK, &INFO);
            LWORK=int(WORK[0]);
            WORK.resize(LWORK);
            dtgexc(&WANTQ, &WANTZ, &Nl, A.data(), &Nl, B.data(), &Nl, Q.data(), &Nl, Z.data(), &Nl, &IFST, &ILST, &WORK[0], &LWORK, &INFO);
        }
        //now we compute the eigenvector
        SIDE='R';
        char HOWMNY('S');
        int M(0);
        Matrix<double> Z_I(Nl,Nl);
        bool SELECT[Nl];
        for (int zi=0; zi<Nl; zi++) SELECT[zi]=false;
        SELECT[0]=true;

        WORK.resize(6*Nl);
        dtgevc(&SIDE, &HOWMNY, &SELECT[0], &Nl, A.data(), &Nl, B.data(), &Nl, Q.data(), &LDQ, Z_I.data(), &Nl, &Nl, &M, &WORK[0], &INFO);

        std::vector<RealType> evec(Nl,0);
        for (int i=0; i<Nl; i++) for (int j=0; j<Nl; j++) evec[i] += Z(j,i)*Z_I(0,j);
        for (int i=0; i<Nl; i++) ev[i] = evec[i]/evec[0];
//     for (int i=0; i<Nl; i++) app_log()<<ev[i]<<" ";
//     app_log()<<endl;
        return mappedEigenvalues[0].first;
    }
    else
    {
// OLD ROUTINE. CALCULATES ALL EIGENVECTORS
//   Getting the optimal worksize
        char jl('N');
        char jr('V');
        vector<RealType> alphar(Nl),alphai(Nl),beta(Nl);
        Matrix<RealType> eigenT(Nl,Nl);
        int info;
        int lwork(-1);
        vector<RealType> work(1);

        RealType tt(0);
        int t(1);
        dggev(&jl, &jr, &Nl, A.data(), &Nl, B.data(), &Nl, &alphar[0], &alphai[0], &beta[0],&tt,&t, eigenT.data(), &Nl, &work[0], &lwork, &info);
        lwork=int(work[0]);
        work.resize(lwork);

        //~ //Get an estimate of E_lin
        //~ Matrix<RealType> H_tmp(HamT);
        //~ Matrix<RealType> S_tmp(ST);
        //~ dggev(&jl, &jr, &Nl, H_tmp.data(), &Nl, S_tmp.data(), &Nl, &alphar[0], &alphai[0], &beta[0],&tt,&t, eigenT.data(), &Nl, &work[0], &lwork, &info);
        //~ RealType E_lin(alphar[0]/beta[0]);
        //~ int e_min_indx(0);
        //~ for (int i=1; i<Nl; i++)
        //~ if (E_lin>(alphar[i]/beta[i]))
        //~ {
        //~ E_lin=alphar[i]/beta[i];
        //~ e_min_indx=i;
        //~ }
        dggev(&jl, &jr, &Nl, A.data(), &Nl, B.data(), &Nl, &alphar[0], &alphai[0], &beta[0],&tt,&t, eigenT.data(), &Nl, &work[0], &lwork, &info);
        if (info!=0)
        {
            APP_ABORT("Invalid Matrix Diagonalization Function!");
        }

        vector<std::pair<RealType,int> > mappedEigenvalues(Nl);
        for (int i=0; i<Nl; i++)
        {
            RealType evi(alphar[i]/beta[i]);
            if (abs(evi)<1e10)
            {
                mappedEigenvalues[i].first=evi;
                mappedEigenvalues[i].second=i;
            }
            else
            {
                mappedEigenvalues[i].first=1e100;
                mappedEigenvalues[i].second=i;
            }
        }
        std::sort(mappedEigenvalues.begin(),mappedEigenvalues.end());

        for (int i=0; i<Nl; i++) ev[i] = eigenT(mappedEigenvalues[0].second,i)/eigenT(mappedEigenvalues[0].second,0);
        return mappedEigenvalues[0].first;
    }
}


bool QMCLinearOptimize::nonLinearRescale(std::vector<RealType>& dP, Matrix<RealType> S)
{
    RealType rescale = getNonLinearRescale(dP,S);
    for (int i=1; i<dP.size(); i++) dP[i]*=rescale;
    return true;
};


void QMCLinearOptimize::getNonLinearRange(int& first, int& last)
{
    std::vector<int> types;
    optTarget->getParameterTypes(types);
    first=0;
    last=types.size();

    //assume all non-linear coeffs are together.
    if (types[0]==optimize::LINEAR_P)
    {
        int i(0);
        while ((types[i]==optimize::LINEAR_P)&&(i<types.size())) i++;
        first=i;
    }
    else
    {
        int i(1);
        while ((types[i]!=optimize::LINEAR_P)&&(i<types.size())) i++;
        last=i;
    }
//     returns the number of non-linear parameters.
//     app_log()<<first<<" "<<last<<endl;
};

QMCLinearOptimize::RealType QMCLinearOptimize::getNonLinearRescale(std::vector<RealType>& dP, Matrix<RealType> S)
{
    int first(0),last(0);
    getNonLinearRange(first,last);
    if (first==last) return 1.0;

    RealType D(1.0);
    for (int i=first; i<last; i++) for (int j=first; j<last; j++) D += S(j+1,i+1)*dP[i+1]*dP[j+1];

    D = std::sqrt(std::abs(D));


    vector<RealType> N_i(last-first,0);
    vector<RealType> M_i(last-first,0);
    RealType xi(0.5);
    for (int i=0; i<last-first; i++)
    {
        M_i[i] = xi*D +(1-xi);
        RealType tsumN(0);
        for (int j=first; j<last; j++)
        {
            tsumN += S(i+first+1,j+1)*dP[j+1];
        }
        N_i[i] += (1-xi)*tsumN;
        N_i[i] *= -1.0/M_i[i];
    }

    RealType rescale(1);
    for (int j=0; j<last-first; j++) rescale -= N_i[j]*dP[j+first+1];
    rescale = 1.0/rescale;
    return rescale;
};

QMCLinearOptimize::RealType QMCLinearOptimize::getSplitEigenvectors(int first, int last, Matrix<RealType>& FullLeft, Matrix<RealType>& FullRight, vector<RealType>& FullEV, vector<RealType>& LocalEV, string CSF_Option, bool& CSF_scaled)
{
    vector<RealType> GEVSplitDirection(N,0);
    RealType returnValue;
    int N_nonlin=last-first;
    int N_lin   =N-N_nonlin-1;

//  matrices are one larger than parameter sets
    int M_nonlin=N_nonlin+1;
    int M_lin   =N_lin+1;
//  index mapping for the matrices
    int J_begin(first+1), J_end(last+1);
    int CSF_begin(1), CSF_end(first+1);
    if (first==0)
    {
        CSF_begin=last+1;
        CSF_end=N;
    }
//the Mini matrix composed of just the Nonlinear terms
    Matrix<RealType> LeftTJ(M_nonlin,M_nonlin), RightTJ(M_nonlin,M_nonlin);

//                     assume all jastrow parameters are together either first or last
    LeftTJ(0,0) =  FullLeft(0,0);
    RightTJ(0,0)= FullRight(0,0);
    for (int i=J_begin; i<J_end; i++)
    {
        LeftTJ(i-J_begin+1,0) =  FullLeft(i,0);
        RightTJ(i-J_begin+1,0)= FullRight(i,0);
        LeftTJ(0,i-J_begin+1) =  FullLeft(0,i);
        RightTJ(0,i-J_begin+1)= FullRight(0,i);
        for (int j=J_begin; j<J_end; j++)
        {
            LeftTJ(i-J_begin+1,j-J_begin+1) =  FullLeft(i,j);
            RightTJ(i-J_begin+1,j-J_begin+1)= FullRight(i,j);
        }
    }

    vector<RealType> J_parms(M_nonlin);
    myTimers[2]->start();
    RealType lowest_J_EV =getLowestEigenvector(LeftTJ,RightTJ,J_parms);
    myTimers[2]->stop();

//the Mini matrix composed of just the Linear terms
    Matrix<RealType> LeftTCSF(M_lin,M_lin), RightTCSF(M_lin,M_lin);

    LeftTCSF(0,0) =  FullLeft(0,0);
    RightTCSF(0,0)= FullRight(0,0);
    for (int i=CSF_begin; i<CSF_end; i++)
    {
        LeftTCSF(i-CSF_begin+1,0) =  FullLeft(i,0);
        RightTCSF(i-CSF_begin+1,0)= FullRight(i,0);
        LeftTCSF(0,i-CSF_begin+1) =  FullLeft(0,i);
        RightTCSF(0,i-CSF_begin+1)= FullRight(0,i);
        for (int j=CSF_begin; j<CSF_end; j++)
        {
            LeftTCSF(i-CSF_begin+1,j-CSF_begin+1) =  FullLeft(i,j);
            RightTCSF(i-CSF_begin+1,j-CSF_begin+1)= FullRight(i,j);
        }
    }
    vector<RealType> CSF_parms(M_lin);
    myTimers[2]->start();
    RealType lowest_CSF_EV =getLowestEigenvector(LeftTCSF,RightTCSF,CSF_parms);
    myTimers[2]->stop();

// //                   Now we have both eigenvalues and eigenvectors
//                   app_log()<<" Jastrow eigenvalue: "<<lowest_J_EV<<endl;
//                   app_log()<<"     CSF eigenvalue: "<<lowest_CSF_EV<<endl;

//                We can rescale the matrix and re-solve the whole thing or take the CSF parameters
//                  as solved in the matrix and opt the Jastrow instead

    if (CSF_Option=="freeze")
    {
        returnValue=min(lowest_J_EV,lowest_CSF_EV);
//                   Line minimize for the nonlinear components
        for (int i=J_begin; i<J_end; i++) GEVSplitDirection[i]=J_parms[i-J_begin+1];

//                   freeze the CSF components at this minimum
        for (int i=CSF_begin; i<CSF_end; i++) LocalEV[i-1]=CSF_parms[i-CSF_begin+1];

        FullEV[0]=1.0;
        for (int i=J_begin; i<J_end; i++) FullEV[i]=GEVSplitDirection[i];
    }
    else if (CSF_Option=="rescale")
    {
        RealType matrixRescaler=std::sqrt(std::abs(lowest_CSF_EV/lowest_J_EV));
        for (int i=0; i<N; i++)
            for (int j=0; j<N; j++)
            {
                if ((i>=J_begin)&&(i<J_end))
                {
                    FullLeft(i,j) *=matrixRescaler;
                    FullRight(i,j)*=matrixRescaler;
                }

                if ((j>=J_begin)&&(j<J_end))
                {
                    FullLeft(i,j) *=matrixRescaler;
                    FullRight(i,j)*=matrixRescaler;
                }
            }

        myTimers[2]->start();
        returnValue =getLowestEigenvector(FullLeft,FullRight,FullEV);
        myTimers[2]->stop();
    }
    else if (CSF_Option =="stability")
    {
//       just return the value of the CSF part
        if (lowest_J_EV>lowest_CSF_EV)
            CSF_scaled=true;
        else
            CSF_scaled=false;
        returnValue=min(lowest_J_EV,lowest_CSF_EV);
    }
    return returnValue;
};

/** Parses the xml input file for parameter definitions for the wavefunction optimization.
* @param q current xmlNode
* @return true if successful
*/
bool
QMCLinearOptimize::put(xmlNodePtr q)
{
    string useGPU("no");
    string vmcMove("pbyp");
    OhmmsAttributeSet oAttrib;
    oAttrib.add(useGPU,"gpu");
    oAttrib.add(vmcMove,"move");
    oAttrib.put(q);

    xmlNodePtr qsave=q;
    xmlNodePtr cur=qsave->children;


    int pid=OHMMS::Controller->rank();
    while (cur != NULL)
    {
        string cname((const char*)(cur->name));
        if (cname == "mcwalkerset")
        {
            mcwalkerNodePtr.push_back(cur);
        }
        else if (cname == "optimizer")
        {
            xmlChar* att= xmlGetProp(cur,(const xmlChar*)"method");
            if (att)
            {
                optmethod = (const char*)att;
            }
            optNode=cur;
        }
        else if (cname == "optimize")
        {
            xmlChar* att= xmlGetProp(cur,(const xmlChar*)"method");
            if (att)
            {
                optmethod = (const char*)att;
            }
        }
        cur=cur->next;
    }
    //no walkers exist, add 10
    if (W.getActiveWalkers() == 0) addWalkers(omp_get_max_threads());

    NumOfVMCWalkers=W.getActiveWalkers();

    //create VMC engine
    if (vmcEngine ==0)
    {
#if defined (QMC_CUDA)
        if (useGPU == "yes")
            vmcEngine = new VMCcuda(W,Psi,H);
        else
#endif
//#if defined(ENABLE_OPENMP)
//        if(omp_get_max_threads()>1)
//          vmcEngine = new VMCSingleOMP(W,Psi,H,hamPool);
//        else
//#endif
//          vmcEngine = new VMCSingle(W,Psi,H);
            vmcEngine = new VMCSingleOMP(W,Psi,H,hamPool);
        vmcEngine->setUpdateMode(vmcMove[0] == 'p');
        vmcEngine->initCommunicator(myComm);
    }
    vmcEngine->setStatus(RootName,h5FileRoot,AppendRun);
    vmcEngine->process(qsave);

    bool success=true;

    if (optTarget == 0)
    {
#if defined (QMC_CUDA)
        if (useGPU == "yes")
            optTarget = new QMCCostFunctionCUDA(W,Psi,H,hamPool);
        else
#endif
#if defined(ENABLE_OPENMP)
            if (omp_get_max_threads()>1)
            {
                optTarget = new QMCCostFunctionOMP(W,Psi,H,hamPool);
            }
            else
#endif
                optTarget = new QMCCostFunctionSingle(W,Psi,H);

        optTarget->setStream(&app_log());
        success=optTarget->put(q);
    }
    return success;
}

void QMCLinearOptimize::resetComponents(xmlNodePtr cur)
{
    exp0=-16;
    m_param.put(cur);
    optTarget->put(cur);
    vmcEngine->resetComponents(cur);
}
}
/***************************************************************************
* $RCSfile$   $Author: jnkim $
* $Revision: 1286 $   $Date: 2006-08-17 12:33:18 -0500 (Thu, 17 Aug 2006) $
* $Id: QMCLinearOptimize.cpp 1286 2006-08-17 17:33:18Z jnkim $
***************************************************************************/
