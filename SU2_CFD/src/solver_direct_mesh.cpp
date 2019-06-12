/*!
 * \file solver_direct_mesh.cpp
 * \brief Main subroutines to solve moving meshes using a pseudo-linear elastic approach.
 * \author R. Sanchez
 * \version 6.1.0 "Falcon"
 *
 * The current SU2 release has been coordinated by the
 * SU2 International Developers Society <www.su2devsociety.org>
 * with selected contributions from the open-source community.
 *
 * The main research teams contributing to the current release are:
 *  - Prof. Juan J. Alonso's group at Stanford University.
 *  - Prof. Piero Colonna's group at Delft University of Technology.
 *  - Prof. Nicolas R. Gauger's group at Kaiserslautern University of Technology.
 *  - Prof. Alberto Guardone's group at Polytechnic University of Milan.
 *  - Prof. Rafael Palacios' group at Imperial College London.
 *  - Prof. Vincent Terrapon's group at the University of Liege.
 *  - Prof. Edwin van der Weide's group at the University of Twente.
 *  - Lab. of New Concepts in Aeronautics at Tech. Institute of Aeronautics.
 *
 * Copyright 2012-2018, Francisco D. Palacios, Thomas D. Economon,
 *                      Tim Albring, and the SU2 contributors.
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/solver_structure.hpp"
#include "../../Common/include/adt_structure.hpp"

CMeshSolver::CMeshSolver(CGeometry *geometry, CConfig *config) : CFEASolver(true) {

    /*--- Initialize some booleans that determine the kind of problem at hand. ---*/

    time_domain = config->GetTime_Domain();
    multizone = config->GetMultizone_Problem();

    /*--- Determine if the stiffness per-element is set ---*/
    switch (config->GetDeform_Stiffness_Type()) {
    case INVERSE_VOLUME:
    case SOLID_WALL_DISTANCE:
      stiffness_set = false;
      break;
    case CONSTANT_STIFFNESS:
      stiffness_set = true;
      break;
    }

    /*--- Initialize the number of spatial dimensions, length of the state
     vector (same as spatial dimensions for grid deformation), and grid nodes. ---*/

    unsigned short iDim, jDim;
    unsigned long iPoint, iElem;

    nDim         = geometry->GetnDim();
    nVar         = geometry->GetnDim();
    nPoint       = geometry->GetnPoint();
    nPointDomain = geometry->GetnPointDomain();
    nElement     = geometry->GetnElem();

    valResidual = 0.0;

    MinVolume_Ref = 0.0;
    MinVolume_Curr = 0.0;

    MaxVolume_Ref = 0.0;
    MaxVolume_Curr = 0.0;

    /*--- Initialize the node structure ---*/
    bool isVertex;
    long iVertex;
    Coordinate = new su2double[nDim];
    node       = new CVariable*[nPoint];
    for (iPoint = 0; iPoint < nPoint; iPoint++){

      /*--- We store directly the reference coordinates ---*/
      for (iDim = 0; iDim < nDim; iDim++)
        Coordinate[iDim] = geometry->node[iPoint]->GetCoord(iDim);

      /*--- In principle, the node is not at the boundary ---*/
      isVertex = false;
      /*--- Looping over all markers ---*/
      for (unsigned short iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {

        /*--- If the marker is flagged as moving, retrieve the node vertex ---*/
        if (config->GetMarker_All_Moving(iMarker) == YES) iVertex = geometry->node[iPoint]->GetVertex(iMarker);
        else iVertex = -1;

        if (iVertex != -1){isVertex = true; break;}
      }

      /*--- Temporarily, keep everything the same ---*/
      if (isVertex) node[iPoint] = new CMeshBoundVariable(Coordinate, nDim, config);
      else          node[iPoint] = new CMeshVariable(Coordinate, nDim, config);

    }

    /*--- Initialize the element structure ---*/
    element = new CMeshElement[nElement];
    for (iElem = 0; iElem < nElement; iElem++)
        element[iElem] = CMeshElement();

    Residual = new su2double[nDim];   for (iDim = 0; iDim < nDim; iDim++) Residual[iDim] = 0.0;
    Solution = new su2double[nDim];   for (iDim = 0; iDim < nDim; iDim++) Solution[iDim] = 0.0;

    /*--- Stress contribution to the node i ---*/
    Res_Stress_i = new su2double[nVar];

    /*--- Initialize matrix, solution, and r.h.s. structures for the linear solver. ---*/

    LinSysSol.Initialize(nPoint, nPointDomain, nVar, 0.0);
    LinSysRes.Initialize(nPoint, nPointDomain, nVar, 0.0);
    Jacobian.Initialize(nPoint, nPointDomain, nVar, nVar, false, geometry, config);

    /*--- Structural parameters ---*/

    E      = config->GetDeform_ElasticityMod();
    Nu     = config->GetDeform_PoissonRatio();

    Mu     = E / (2.0*(1.0 + Nu));
    Lambda = Nu*E/((1.0+Nu)*(1.0-2.0*Nu));

    /*--- Element container structure ---*/

    /*--- First level: only the FEA_TERM is considered ---*/
    element_container = new CElement** [1];
    element_container[FEA_TERM] = new CElement* [MAX_FE_KINDS];

    /*--- Initialize all subsequent levels ---*/
    for (unsigned short iKind = 0; iKind < MAX_FE_KINDS; iKind++) {
      element_container[FEA_TERM][iKind] = NULL;
    }

    if (nDim == 2) {
        element_container[FEA_TERM][EL_TRIA] = new CTRIA1(nDim, config);
        element_container[FEA_TERM][EL_QUAD] = new CQUAD4(nDim, config);
    }
    else if (nDim == 3) {
        element_container[FEA_TERM][EL_TETRA] = new CTETRA1(nDim, config);
        element_container[FEA_TERM][EL_HEXA]  = new CHEXA8(nDim, config);
        element_container[FEA_TERM][EL_PYRAM] = new CPYRAM5(nDim, config);
        element_container[FEA_TERM][EL_PRISM] = new CPRISM6(nDim, config);
    }

    /*--- Matrices to impose boundary conditions ---*/

    mZeros_Aux = new su2double *[nDim];
    mId_Aux    = new su2double *[nDim];
    for(iDim = 0; iDim < nDim; iDim++){
      mZeros_Aux[iDim] = new su2double[nDim];
      mId_Aux[iDim]    = new su2double[nDim];
    }

    for(iDim = 0; iDim < nDim; iDim++){
      for (jDim = 0; jDim < nDim; jDim++){
        mZeros_Aux[iDim][jDim] = 0.0;
        mId_Aux[iDim][jDim]    = 0.0;
      }
      mId_Aux[iDim][iDim] = 1.0;
    }

    /*--- Term ij of the Jacobian ---*/

    Jacobian_ij = new su2double*[nDim];
    for (iDim = 0; iDim < nDim; iDim++) {
      Jacobian_ij[iDim] = new su2double [nDim];
      for (jDim = 0; jDim < nDim; jDim++) {
        Jacobian_ij[iDim][jDim] = 0.0;
      }
    }

    unsigned short iVar;

    /*--- Initialize the BGS residuals in multizone problems. ---*/
    if (config->GetMultizone_Residual()){

      Residual_BGS      = new su2double[nVar];         for (iVar = 0; iVar < nVar; iVar++) Residual_BGS[iVar]  = 0.0;
      Residual_Max_BGS  = new su2double[nVar];         for (iVar = 0; iVar < nVar; iVar++) Residual_Max_BGS[iVar]  = 0.0;

      /*--- Define some structures for locating max residuals ---*/

      Point_Max_BGS       = new unsigned long[nVar];  for (iVar = 0; iVar < nVar; iVar++) Point_Max_BGS[iVar]  = 0;
      Point_Max_Coord_BGS = new su2double*[nVar];
      for (iVar = 0; iVar < nVar; iVar++) {
        Point_Max_Coord_BGS[iVar] = new su2double[nDim];
        for (iDim = 0; iDim < nDim; iDim++) Point_Max_Coord_BGS[iVar][iDim] = 0.0;
      }
    }


    /*--- Allocate element properties - only the index, to allow further integration with CFEASolver on a later stage ---*/
    element_properties = new CProperty*[nElement];
    for (iElem = 0; iElem < nElement; iElem++){
      element_properties[iElem] = new CProperty(iElem);
    }

    /*--- Compute the element volumes using the reference coordinates ---*/
    SetMinMaxVolume(geometry, config, false);

    /*--- Compute the wall distance using the reference coordinates ---*/
    SetWallDistance(geometry, config);

}

CMeshSolver::~CMeshSolver(void) {

  unsigned short iDim, iVar;

  delete [] Residual;
  delete [] Solution;

  for (iDim = 0; iDim < nDim; iDim++) {
    delete [] mZeros_Aux[iDim];
    delete [] mId_Aux[iDim];
    delete [] Jacobian_ij[iDim];
  }
  delete [] mZeros_Aux;
  delete [] mId_Aux;
  delete [] Jacobian_ij;

  if (element_container != NULL) {
    for (unsigned short jVar = 0; jVar < MAX_FE_KINDS; jVar++) {
       if (element_container[FEA_TERM][jVar] != NULL) delete element_container[FEA_TERM][jVar];
    }
    delete [] element_container[FEA_TERM];
    delete [] element_container;
  }

}

void CMeshSolver::SetMinMaxVolume(CGeometry *geometry, CConfig *config, bool updated) {

  unsigned long iElem, ElemCounter = 0;
  unsigned short iNode, iDim, nNodes = 0;
  unsigned long indexNode[8]={0,0,0,0,0,0,0,0};
  su2double val_Coord;
  su2double MaxVolume, MinVolume;
  int EL_KIND = 0;

  bool discrete_adjoint = config->GetDiscrete_Adjoint();

  bool RightVol = true;

  su2double ElemVolume;

  if ((rank == MASTER_NODE) && (!discrete_adjoint))
    cout << "Computing volumes of the grid elements." << endl;

  MaxVolume = -1E22; MinVolume = 1E22;

  /*--- Loops over all the elements ---*/

  for (iElem = 0; iElem < geometry->GetnElem(); iElem++) {

    if (geometry->elem[iElem]->GetVTK_Type() == TRIANGLE)      {nNodes = 3; EL_KIND = EL_TRIA;}
    if (geometry->elem[iElem]->GetVTK_Type() == QUADRILATERAL) {nNodes = 4; EL_KIND = EL_QUAD;}
    if (geometry->elem[iElem]->GetVTK_Type() == TETRAHEDRON)   {nNodes = 4; EL_KIND = EL_TETRA;}
    if (geometry->elem[iElem]->GetVTK_Type() == PYRAMID)       {nNodes = 5; EL_KIND = EL_PYRAM;}
    if (geometry->elem[iElem]->GetVTK_Type() == PRISM)         {nNodes = 6; EL_KIND = EL_PRISM;}
    if (geometry->elem[iElem]->GetVTK_Type() == HEXAHEDRON)    {nNodes = 8; EL_KIND = EL_HEXA;}

    /*--- For the number of nodes, we get the coordinates from the connectivity matrix and the geometry structure ---*/

    for (iNode = 0; iNode < nNodes; iNode++) {

      indexNode[iNode] = geometry->elem[iElem]->GetNode(iNode);

      /*--- Compute the volume with the reference or with the current coordinates ---*/
      for (iDim = 0; iDim < nDim; iDim++) {
        if (updated) val_Coord = node[indexNode[iNode]]->GetMesh_Coord(iDim) 
                               + node[indexNode[iNode]]->GetSolution(iDim);
        else val_Coord = node[indexNode[iNode]]->GetMesh_Coord(iDim);
        element_container[FEA_TERM][EL_KIND]->SetRef_Coord(val_Coord, iNode, iDim);
      }
    }

    /*--- Compute the volume of the element (or the area in 2D cases ) ---*/

    if (nDim == 2)  ElemVolume = element_container[FEA_TERM][EL_KIND]->ComputeArea();
    else            ElemVolume = element_container[FEA_TERM][EL_KIND]->ComputeVolume();

    RightVol = true;
    if (ElemVolume < 0.0) RightVol = false;

    MaxVolume = max(MaxVolume, ElemVolume);
    MinVolume = min(MinVolume, ElemVolume);
    if (updated) element[iElem].SetCurr_Volume(ElemVolume);
    else element[iElem].SetRef_Volume(ElemVolume);

    if (!RightVol) ElemCounter++;

  }

#ifdef HAVE_MPI
  unsigned long ElemCounter_Local = ElemCounter; ElemCounter = 0;
  su2double MaxVolume_Local = MaxVolume; MaxVolume = 0.0;
  su2double MinVolume_Local = MinVolume; MinVolume = 0.0;
  SU2_MPI::Allreduce(&ElemCounter_Local, &ElemCounter, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
  SU2_MPI::Allreduce(&MaxVolume_Local, &MaxVolume, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  SU2_MPI::Allreduce(&MinVolume_Local, &MinVolume, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
#endif

  /*--- Volume from  0 to 1 ---*/
  for (iElem = 0; iElem < geometry->GetnElem(); iElem++) {
    if (updated){
      ElemVolume = element[iElem].GetCurr_Volume()/MaxVolume;
      element[iElem].SetCurr_Volume(ElemVolume);
    }
    else{
      ElemVolume = element[iElem].GetRef_Volume()/MaxVolume;
      element[iElem].SetRef_Volume(ElemVolume);
    }
  }

  /*--- Store the maximum and minimum volume ---*/
  if (updated){
    MaxVolume_Curr = MaxVolume;
    MinVolume_Curr = MinVolume;
  }
  else{
    MaxVolume_Ref = MaxVolume;
    MinVolume_Ref = MinVolume;
  }

  if ((ElemCounter != 0) && (rank == MASTER_NODE))
    cout <<"There are " << ElemCounter << " elements with negative volume.\n" << endl;

}

void CMeshSolver::SetWallDistance(CGeometry *geometry, CConfig *config) {

  unsigned long nVertex_SolidWall, ii, jj, iVertex, iPoint, pointID;
  unsigned long iElem, PointCorners[8];
  unsigned short iNodes, nNodes;
  unsigned short iMarker, iDim;
  su2double dist, MaxDistance_Local, MinDistance_Local;
  su2double nodeDist, ElemDist;
  int rankID;

  /*--- Initialize min and max distance ---*/

  MaxDistance = -1E22; MinDistance = 1E22;

  /*--- Compute the total number of nodes on no-slip boundaries ---*/

  nVertex_SolidWall = 0;
  for(iMarker=0; iMarker<config->GetnMarker_All(); ++iMarker) {
    if( (config->GetMarker_All_KindBC(iMarker) == EULER_WALL ||
         config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX)  ||
       (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL) ) {
      nVertex_SolidWall += geometry->GetnVertex(iMarker);
    }
  }

  /*--- Allocate the vectors to hold boundary node coordinates
   and its local ID. ---*/

  vector<su2double>     Coord_bound(nDim*nVertex_SolidWall);
  vector<unsigned long> PointIDs(nVertex_SolidWall);

  /*--- Retrieve and store the coordinates of the no-slip boundary nodes
   and their local point IDs. ---*/

  ii = 0; jj = 0;
  for (iMarker=0; iMarker<config->GetnMarker_All(); ++iMarker) {
    if ( (config->GetMarker_All_KindBC(iMarker) == EULER_WALL ||
         config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX)  ||
       (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL) ) {
      for (iVertex=0; iVertex<geometry->GetnVertex(iMarker); ++iVertex) {
        iPoint = geometry->vertex[iMarker][iVertex]->GetNode();
        PointIDs[jj++] = iPoint;
        for (iDim=0; iDim<nDim; ++iDim){
          Coord_bound[ii++] = node[iPoint]->GetMesh_Coord(iDim);
        }
      }
    }
  }

  /*--- Build the ADT of the boundary nodes. ---*/

  CADTPointsOnlyClass WallADT(nDim, nVertex_SolidWall, Coord_bound.data(),
                              PointIDs.data(), true);


  /*--- Loop over all interior mesh nodes and compute the distances to each
   of the no-slip boundary nodes. Store the minimum distance to the wall
   for each interior mesh node. ---*/

  if( WallADT.IsEmpty() ) {

    /*--- No solid wall boundary nodes in the entire mesh.
     Set the wall distance to zero for all nodes. ---*/

    for (iPoint=0; iPoint<geometry->GetnPoint(); ++iPoint)
      geometry->node[iPoint]->SetWall_Distance(0.0);
  }
  else {

    /*--- Solid wall boundary nodes are present. Compute the wall
     distance for all nodes. ---*/

    for(iPoint=0; iPoint< nPoint; ++iPoint) {

      WallADT.DetermineNearestNode(node[iPoint]->GetMesh_Coord(), dist,
                                   pointID, rankID);
      node[iPoint]->SetWallDistance(dist);

      MaxDistance = max(MaxDistance, dist);

      /*--- To discard points on the surface we use > EPS ---*/

      if (sqrt(dist) > EPS)  MinDistance = min(MinDistance, dist);

    }

    MaxDistance_Local = MaxDistance; MaxDistance = 0.0;
    MinDistance_Local = MinDistance; MinDistance = 0.0;

#ifdef HAVE_MPI
    SU2_MPI::Allreduce(&MaxDistance_Local, &MaxDistance, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    SU2_MPI::Allreduce(&MinDistance_Local, &MinDistance, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
#else
    MaxDistance = MaxDistance_Local;
    MinDistance = MinDistance_Local;
#endif

  }

  /*--- Normalize distance from 0 to 1 ---*/
  for (iPoint=0; iPoint < nPoint; ++iPoint) {
    nodeDist = node[iPoint]->GetWallDistance()/MaxDistance;
    node[iPoint]->SetWallDistance(nodeDist);
  }

  /*--- Compute the element distances ---*/

  for (iElem = 0; iElem < nElement; iElem++) {

    if (geometry->elem[iElem]->GetVTK_Type() == TRIANGLE)      nNodes = 3;
    if (geometry->elem[iElem]->GetVTK_Type() == QUADRILATERAL) nNodes = 4;
    if (geometry->elem[iElem]->GetVTK_Type() == TETRAHEDRON)   nNodes = 4;
    if (geometry->elem[iElem]->GetVTK_Type() == PYRAMID)       nNodes = 5;
    if (geometry->elem[iElem]->GetVTK_Type() == PRISM)         nNodes = 6;
    if (geometry->elem[iElem]->GetVTK_Type() == HEXAHEDRON)    nNodes = 8;

    for (iNodes = 0; iNodes < nNodes; iNodes++) {
      PointCorners[iNodes] = geometry->elem[iElem]->GetNode(iNodes);
    }

    /*--- Average the distance of the nodes in the element ---*/

    ElemDist = 0.0;
    for (iNodes = 0; iNodes < nNodes; iNodes++){
      ElemDist += node[PointCorners[iNodes]]->GetWallDistance();
    }
    ElemDist = ElemDist/(su2double)nNodes;

    element[iElem].SetWallDistance(ElemDist);

  }

}

void CMeshSolver::SetMesh_Stiffness(CGeometry **geometry, CNumerics **numerics, CConfig *config){

  unsigned long iElem;

  if(!stiffness_set){
    for (iElem = 0; iElem < nElement; iElem++) {

      switch (config->GetDeform_Stiffness_Type()) {
      /*--- Stiffness inverse of the volume of the element ---*/
      case INVERSE_VOLUME: E = 1.0 / element[iElem].GetRef_Volume();  break;
      /*--- Stiffness inverse of the distance of the element to the closest wall ---*/
      case SOLID_WALL_DISTANCE: E = 1.0 / element[iElem].GetWallDistance(); break;
      }

      /*--- Set the element elastic properties in the numerics container ---*/
      numerics[FEA_TERM]->SetMeshElasticProperties(iElem, E);

    }

    stiffness_set = true;
  }

}

void CMeshSolver::DeformMesh(CGeometry **geometry, CNumerics **numerics, CConfig *config){

  bool discrete_adjoint = config->GetDiscrete_Adjoint();

  if (multizone) SetSolution_Old();

  /*--- Initialize sparse matrix ---*/
  Jacobian.SetValZero();

  /*--- Compute the minimum and maximum area/volume for the mesh. ---*/
  if ((rank == MASTER_NODE) && (!discrete_adjoint)) {
    if (nDim == 2) cout << scientific << "Min. area in the undeformed mesh: "<< MinVolume_Ref <<", max. area: " << MaxVolume_Ref <<"." << endl;
    else           cout << scientific << "Min. volume in the undeformed mesh: "<< MinVolume_Ref <<", max. volume: " << MaxVolume_Ref <<"." << endl;
  }

  /*--- Compute the stiffness matrix. ---*/
  Compute_StiffMatrix(geometry[MESH_0], numerics, config);

  /*--- Initialize vectors and clean residual ---*/
  LinSysSol.SetValZero();
  LinSysRes.SetValZero();

  /*--- Impose boundary conditions (all of them are ESSENTIAL BC's - displacements). ---*/
  SetBoundaryDisplacements(geometry[MESH_0], numerics[FEA_TERM], config);

  /*--- Solve the linear system. ---*/
  Solve_System(geometry[MESH_0], config);

  /*--- Update the grid coordinates and cell volumes using the solution
     of the linear system (usol contains the x, y, z displacements). ---*/
  UpdateGridCoord(geometry[MESH_0], config);

  /*--- Update the dual grid. ---*/
  UpdateDualGrid(geometry[MESH_0], config);

  /*--- Check for failed deformation (negative volumes). ---*/
  /*--- In order to do this, we recompute the minimum and maximum area/volume for the mesh using the current coordinates. ---*/
  SetMinMaxVolume(geometry[MESH_0], config, true);

  if ((rank == MASTER_NODE) && (!discrete_adjoint)) {
    cout << scientific << "Linear solver iter.: " << IterLinSol << ". ";
    if (nDim == 2) cout << "Min. area in the deformed mesh: " << MinVolume_Curr << ". Error: " << valResidual << "." << endl;
    else cout << "Min. volume in the deformed mesh: " << MinVolume_Curr << ". Error: " << valResidual << "." << endl;
  }

  /*--- The Grid Velocity is only computed if the problem is time domain ---*/
  if (time_domain) ComputeGridVelocity(geometry[MESH_0], config);

  /*--- Update the multigrid structure. ---*/
  UpdateMultiGrid(geometry, config);

}

void CMeshSolver::UpdateGridCoord(CGeometry *geometry, CConfig *config){

  unsigned short iDim;
  unsigned long iPoint, total_index;
  su2double val_disp, val_coord;

  /*--- Update the grid coordinates using the solution of the linear system ---*/

  /*--- LinSysSol contains the absolute x, y, z displacements. ---*/
  for (iPoint = 0; iPoint < nPoint; iPoint++){
    for (iDim = 0; iDim < nDim; iDim++) {
      total_index = iPoint*nDim + iDim;
      /*--- Retrieve the displacement from the solution of the linear system ---*/
      val_disp = LinSysSol[total_index];
      /*--- Store the displacement of the mesh node ---*/
      node[iPoint]->SetSolution(iDim, val_disp);
      /*--- Compute the current coordinate as Mesh_Coord + Displacement ---*/
      val_coord = node[iPoint]->GetMesh_Coord(iDim) + val_disp;
      /*--- Update the geometry container ---*/
      geometry->node[iPoint]->SetCoord(iDim, val_coord);
    }
  }

  /*--- LinSysSol contains the non-transformed displacements in the periodic halo cells.
   Hence we still need a communication of the transformed coordinates, otherwise periodicity
   is not maintained. ---*/
  geometry->InitiateComms(geometry, config, COORDINATES);
  geometry->CompleteComms(geometry, config, COORDINATES);

  /*--- In the same way, communicate the displacements in the solver to make sure the halo
   nodes receive the correct value of the displacement. ---*/
  InitiateComms(geometry, config, SOLUTION);
  CompleteComms(geometry, config, SOLUTION);

}

void CMeshSolver::UpdateDualGrid(CGeometry *geometry, CConfig *config){

  /*--- After moving all nodes, update the dual mesh. Recompute the edges and
   dual mesh control volumes in the domain and on the boundaries. ---*/

  geometry->SetCoord_CG();
  geometry->SetControlVolume(config, UPDATE);
  geometry->SetBoundControlVolume(config, UPDATE);
  geometry->SetMaxLength(config);

}

void CMeshSolver::ComputeGridVelocity(CGeometry *geometry, CConfig *config){

  /*--- Local variables ---*/

  su2double *Disp_nP1 = NULL, *Disp_n = NULL, *Disp_nM1 = NULL;
  su2double TimeStep, GridVel = 0.0;
  unsigned long iPoint;
  unsigned short iDim;

  /*--- Compute the velocity of each node in the domain of the current rank
   (halo nodes are not computed as the grid velocity is later communicated) ---*/
  for (iPoint = 0; iPoint < nPointDomain; iPoint++) {

    /*--- Coordinates of the current point at n+1, n, & n-1 time levels ---*/

    Disp_nM1 = node[iPoint]->GetSolution_time_n1();
    Disp_n   = node[iPoint]->GetSolution_time_n();
    Disp_nP1 = node[iPoint]->GetSolution();

    /*--- Unsteady time step ---*/

    TimeStep = config->GetDelta_UnstTimeND();

    /*--- Compute mesh velocity with 1st or 2nd-order approximation ---*/

    for (iDim = 0; iDim < nDim; iDim++) {
      if (config->GetUnsteady_Simulation() == DT_STEPPING_1ST)
        GridVel = ( Disp_nP1[iDim] - Disp_n[iDim] ) / TimeStep;
      if (config->GetUnsteady_Simulation() == DT_STEPPING_2ND)
        GridVel = ( 3.0*Disp_nP1[iDim] - 4.0*Disp_n[iDim]
                    +  1.0*Disp_nM1[iDim] ) / (2.0*TimeStep);

      /*--- Store grid velocity for this point ---*/

      geometry->node[iPoint]->SetGridVel(iDim, GridVel);

    }
  }

  /*--- The velocity was computed for nPointDomain, now we communicate it ---*/
  //geometry->Set_MPI_GridVel(config);
  geometry->InitiateComms(geometry, config, GRID_VELOCITY);
  geometry->CompleteComms(geometry, config, GRID_VELOCITY);


}

void CMeshSolver::UpdateMultiGrid(CGeometry **geometry, CConfig *config){

  unsigned short iMGfine, iMGlevel, nMGlevel = config->GetnMGLevels();

  /*--- Update the multigrid structure after moving the finest grid,
   including computing the grid velocities on the coarser levels
   when the problem is solved in unsteady conditions. ---*/

  for (iMGlevel = 1; iMGlevel <= nMGlevel; iMGlevel++) {
    iMGfine = iMGlevel-1;
    geometry[iMGlevel]->SetControlVolume(config, geometry[iMGfine], UPDATE);
    geometry[iMGlevel]->SetBoundControlVolume(config, geometry[iMGfine],UPDATE);
    geometry[iMGlevel]->SetCoord(geometry[iMGfine]);
    if (time_domain)
      geometry[iMGlevel]->SetRestricted_GridVelocity(geometry[iMGfine], config);
  }

}

void CMeshSolver::SetBoundaryDisplacements(CGeometry *geometry, CNumerics *numerics, CConfig *config){

  unsigned short iMarker;

  /*--- As initialization, move all the interfaces defined as moving. ---*/

  for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
    if (config->GetMarker_All_Moving(iMarker) == YES) {

      /*--- Impose the boundary condition ---*/
      BC_Moving(geometry, numerics, config, iMarker);

    }
  }

  /*--- Then, impose zero displacements of all non-moving surfaces (also at nodes in multiple moving/non-moving boundaries) ---*/
  /*--- Exceptions: symmetry plane, the receive boundaries and periodic boundaries should get a different treatment. ---*/
  for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
    if ((config->GetMarker_All_Moving(iMarker) == NO) &&
        ((config->GetMarker_All_KindBC(iMarker) != SYMMETRY_PLANE) &&
         (config->GetMarker_All_KindBC(iMarker) != SEND_RECEIVE) &&
         (config->GetMarker_All_KindBC(iMarker) != PERIODIC_BOUNDARY))) {

         BC_Clamped(geometry, numerics, config, iMarker);

    }
  }

  /*--- Symmetry plane and periodic boundaries are pending. ---*/


}

void CMeshSolver::ComputeBoundary_Displacements(CGeometry *geometry, CConfig *config){

  unsigned short iDim, iMarker;
  unsigned long iNode, iVertex;

  su2double *VarDisp = NULL;

  su2double VarCoord[3] = {0.0, 0.0, 0.0};

  for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
    if (config->GetMarker_All_Moving(iMarker) == YES) {

      for (iVertex = 0; iVertex < geometry->nVertex[iMarker]; iVertex++) {

        /*--- Get node index ---*/
        iNode = geometry->vertex[iMarker][iVertex]->GetNode();

        /*--- Get the displacement on the vertex ---*/
        VarDisp = geometry->vertex[iMarker][iVertex]->GetVarCoord();

        /*--- Add it to the current displacement. This will be replaced in the transfer routines.  ---*/
        for (iDim = 0; iDim < nDim; iDim++){
          VarCoord[iDim] = node[iNode]->GetSolution(iDim) + VarDisp[iDim];
        }

        if (geometry->node[iNode]->GetDomain()) {

          node[iNode]->SetBound_Disp(VarCoord);

        }

      }

    }
  }

}

void CMeshSolver::SetDualTime_Mesh(void){

  unsigned long iPoint;

  for (iPoint = 0; iPoint < nPoint; iPoint++) {
    node[iPoint]->SetSolution_time_n1();
    node[iPoint]->SetSolution_time_n();
  }

}

void CMeshSolver::ComputeResidual_Multizone(CGeometry *geometry, CConfig *config){

  unsigned short iVar;
  unsigned long iPoint;
  su2double residual;

  /*--- Set Residuals to zero ---*/

  for (iVar = 0; iVar < nVar; iVar++){
      SetRes_BGS(iVar,0.0);
      SetRes_Max_BGS(iVar,0.0,0);
  }

  /*--- Set the residuals ---*/
  for (iPoint = 0; iPoint < nPointDomain; iPoint++){
      for (iVar = 0; iVar < nVar; iVar++){
          residual = node[iPoint]->GetSolution(iVar) - node[iPoint]->GetSolution_Old(iVar);
          AddRes_BGS(iVar,residual*residual);
          AddRes_Max_BGS(iVar,fabs(residual),geometry->node[iPoint]->GetGlobalIndex(),geometry->node[iPoint]->GetCoord());
      }
  }

  SetResidual_BGS(geometry, config);

}

void CMeshSolver::LoadRestart(CGeometry **geometry, CSolver ***solver, CConfig *config, int val_iter, bool val_update_geo) {

  /*--- Restart the solution from file information ---*/
  unsigned short iDim;
  unsigned long index;
  bool dual_time = ((config->GetUnsteady_Simulation() == DT_STEPPING_1ST) ||
                    (config->GetUnsteady_Simulation() == DT_STEPPING_2ND));
  bool time_stepping = config->GetUnsteady_Simulation() == TIME_STEPPING;

  su2double curr_coord, displ;

  string UnstExt, text_line;
  ifstream restart_file;

  unsigned short iZone = config->GetiZone();
  unsigned short nZone = config->GetnZone();

  string restart_filename = config->GetSolution_FlowFileName();

  int counter = 0;
  long iPoint_Local = 0; unsigned long iPoint_Global = 0;
  unsigned long iPoint_Global_Local = 0;
  unsigned short rbuf_NotMatching = 0, sbuf_NotMatching = 0;

  /*--- Multizone problems require the number of the zone to be appended. ---*/

  if (nZone > 1)
    restart_filename = config->GetMultizone_FileName(restart_filename, iZone);

  /*--- Modify file name for an unsteady restart ---*/

  if (dual_time || time_stepping)
    restart_filename = config->GetUnsteady_FileName(restart_filename, val_iter);

  /*--- Read the restart data from either an ASCII or binary SU2 file. ---*/

  if (config->GetRead_Binary_Restart()) {
    Read_SU2_Restart_Binary(geometry[MESH_0], config, restart_filename);
  } else {
    Read_SU2_Restart_ASCII(geometry[MESH_0], config, restart_filename);
  }

  /*--- Load data from the restart into correct containers. ---*/

  counter = 0;
  for (iPoint_Global = 0; iPoint_Global < geometry[MESH_0]->GetGlobal_nPointDomain(); iPoint_Global++ ) {

    /*--- Retrieve local index. If this node from the restart file lives
     on the current processor, we will load and instantiate the vars. ---*/

    iPoint_Local = geometry[MESH_0]->GetGlobal_to_Local_Point(iPoint_Global);

    if (iPoint_Local > -1) {

      /*--- We need to store this point's data, so jump to the correct
       offset in the buffer of data from the restart file and load it. ---*/

      index = counter*Restart_Vars[1];
      for (iDim = 0; iDim < nDim; iDim++){
        /*--- Update the coordinates of the mesh ---*/
        curr_coord = Restart_Data[index+iDim];
        geometry[MESH_0]->node[iPoint_Local]->SetCoord(iDim, curr_coord);
        /*--- Store the displacements computed as the current coordinates
         minus the coordinates of the reference mesh file ---*/
        displ = curr_coord - node[iPoint_Local]->GetMesh_Coord(iDim);
        node[iPoint_Local]->SetSolution(iDim, displ);
      }
      iPoint_Global_Local++;

      /*--- Increment the overall counter for how many points have been loaded. ---*/
      counter++;
    }

  }

  /*--- Detect a wrong solution file ---*/

  if (iPoint_Global_Local < nPointDomain) { sbuf_NotMatching = 1; }

#ifndef HAVE_MPI
  rbuf_NotMatching = sbuf_NotMatching;
#else
  SU2_MPI::Allreduce(&sbuf_NotMatching, &rbuf_NotMatching, 1, MPI_UNSIGNED_SHORT, MPI_SUM, MPI_COMM_WORLD);
#endif
  if (rbuf_NotMatching != 0) {
      SU2_MPI::Error(string("The solution file ") + restart_filename + string(" doesn't match with the mesh file!\n") +
                     string("It could be empty lines at the end of the file."), CURRENT_FUNCTION);
  }

  /*--- Communicate the loaded displacements. ---*/
  solver[MESH_0][MESH_SOL]->InitiateComms(geometry[MESH_0], config, SOLUTION);
  solver[MESH_0][MESH_SOL]->CompleteComms(geometry[MESH_0], config, SOLUTION);

  /*--- Communicate the new coordinates at the halos ---*/
  geometry[MESH_0]->InitiateComms(geometry[MESH_0], config, COORDINATES);
  geometry[MESH_0]->CompleteComms(geometry[MESH_0], config, COORDINATES);

  /*--- Recompute the edges and dual mesh control volumes in the
   domain and on the boundaries. ---*/
  UpdateDualGrid(geometry[MESH_0], config);

  /*--- For time-domain problems, we need to compute the grid velocities ---*/
  if (time_domain){
    /*--- Update the old geometry (coordinates n and n-1) ---*/
    Restart_OldGeometry(geometry[MESH_0], config);
    /*--- Once Displacement_n and Displacement_n1 are filled,
     we can compute the Grid Velocity ---*/
    ComputeGridVelocity(geometry[MESH_0], config);
  }

  /*--- Update the multigrid structure after setting up the finest grid,
   including computing the grid velocities on the coarser levels
   when the problem is unsteady. ---*/
  UpdateMultiGrid(geometry, config);

  /*--- Delete the class memory that is used to load the restart. ---*/

  if (Restart_Vars != NULL) delete [] Restart_Vars;
  if (Restart_Data != NULL) delete [] Restart_Data;
  Restart_Vars = NULL; Restart_Data = NULL;

}

void CMeshSolver::Restart_OldGeometry(CGeometry *geometry, CConfig *config) {

  /*--- This function is intended for dual time simulations ---*/

  unsigned long index;

  int Unst_RestartIter;
  ifstream restart_file_n;
  unsigned short iZone = config->GetiZone();
  unsigned short nZone = geometry->GetnZone();
  unsigned short iDim;
  string filename = config->GetSolution_FlowFileName();
  string filename_n;

  /*--- Auxiliary variables for storing the coordinates ---*/
  su2double curr_coord, displ;

  /*--- Variables for reading the restart files ---*/
  int counter = 0;
  long iPoint_Local = 0; unsigned long iPoint_Global = 0;
  unsigned long iPoint_Global_Local = 0;
  unsigned short rbuf_NotMatching = 0, sbuf_NotMatching = 0;

  /*--- Multizone problems require the number of the zone to be appended. ---*/

  if (nZone > 1)
    filename = config->GetMultizone_FileName(filename, iZone);

  /*-------------------------------------------------------------------------------------------*/
  /*----------------------- First, load the restart file for time n ---------------------------*/
  /*-------------------------------------------------------------------------------------------*/

  /*--- Modify file name for an unsteady restart ---*/
  Unst_RestartIter = SU2_TYPE::Int(config->GetUnst_RestartIter())-1;
  filename_n = config->GetUnsteady_FileName(filename, Unst_RestartIter);

  /*--- Read the restart data from either an ASCII or binary SU2 file. ---*/

  if (config->GetRead_Binary_Restart()) {
    Read_SU2_Restart_Binary(geometry, config, filename_n);
  } else {
    Read_SU2_Restart_ASCII(geometry, config, filename_n);
  }

  /*--- Load data from the restart into correct containers. ---*/
  counter = 0;
  for (iPoint_Global = 0; iPoint_Global < geometry->GetGlobal_nPointDomain(); iPoint_Global++ ) {

    /*--- Retrieve local index. If this node from the restart file lives
     on the current processor, we will load and instantiate the vars. ---*/

    iPoint_Local = geometry->GetGlobal_to_Local_Point(iPoint_Global);

    if (iPoint_Local > -1) {

      /*--- We need to store this point's data, so jump to the correct
       offset in the buffer of data from the restart file and load it. ---*/

      index = counter*Restart_Vars[1];
      for (iDim = 0; iDim < nDim; iDim++){
        curr_coord = Restart_Data[index+iDim];
        displ = curr_coord - node[iPoint_Local]->GetMesh_Coord(iDim);
        node[iPoint_Local]->SetSolution_time_n(iDim, displ);
      }
      iPoint_Global_Local++;

      /*--- Increment the overall counter for how many points have been loaded. ---*/
      counter++;
    }

  }

  /*--- Detect a wrong solution file ---*/

  if (iPoint_Global_Local < nPointDomain) { sbuf_NotMatching = 1; }

#ifndef HAVE_MPI
  rbuf_NotMatching = sbuf_NotMatching;
#else
  SU2_MPI::Allreduce(&sbuf_NotMatching, &rbuf_NotMatching, 1, MPI_UNSIGNED_SHORT, MPI_SUM, MPI_COMM_WORLD);
#endif
  if (rbuf_NotMatching != 0) {
      SU2_MPI::Error(string("The solution file ") + filename_n + string(" doesn't match with the mesh file!\n") +
                     string("It could be empty lines at the end of the file."), CURRENT_FUNCTION);
  }

  /*--- Delete the class memory that is used to load the restart. ---*/

  if (Restart_Vars != NULL) delete [] Restart_Vars;
  if (Restart_Data != NULL) delete [] Restart_Data;
  Restart_Vars = NULL; Restart_Data = NULL;

  /*-------------------------------------------------------------------------------------------*/
  /*------------ Now, load the restart file for time n-1, if the simulation is 2nd Order ------*/
  /*-------------------------------------------------------------------------------------------*/

  if (config->GetUnsteady_Simulation() == DT_STEPPING_2ND) {

    ifstream restart_file_n1;
    string filename_n1;

    /*--- Restart the variables for reading the restart files ---*/
    counter = 0; iPoint_Local = 0; iPoint_Global = 0; iPoint_Global_Local = 0;
    rbuf_NotMatching = 0; sbuf_NotMatching = 0;

    /*--- Modify file name for an unsteady restart ---*/
    Unst_RestartIter = SU2_TYPE::Int(config->GetUnst_RestartIter())-2;
    filename_n1 = config->GetUnsteady_FileName(filename, Unst_RestartIter);

    /*--- Read the restart data from either an ASCII or binary SU2 file. ---*/

    if (config->GetRead_Binary_Restart()) {
      Read_SU2_Restart_Binary(geometry, config, filename_n1);
    } else {
      Read_SU2_Restart_ASCII(geometry, config, filename_n1);
    }

    /*--- Load data from the restart into correct containers. ---*/
    counter = 0;
    for (iPoint_Global = 0; iPoint_Global < geometry->GetGlobal_nPointDomain(); iPoint_Global++ ) {

      /*--- Retrieve local index. If this node from the restart file lives
       on the current processor, we will load and instantiate the vars. ---*/

      iPoint_Local = geometry->GetGlobal_to_Local_Point(iPoint_Global);

      if (iPoint_Local > -1) {

        /*--- We need to store this point's data, so jump to the correct
         offset in the buffer of data from the restart file and load it. ---*/

        index = counter*Restart_Vars[1];
        for (iDim = 0; iDim < nDim; iDim++){
          curr_coord = Restart_Data[index+iDim];
          displ = curr_coord - node[iPoint_Local]->GetMesh_Coord(iDim);
          node[iPoint_Local]->SetSolution_time_n1(iDim, displ);
        }
        iPoint_Global_Local++;

        /*--- Increment the overall counter for how many points have been loaded. ---*/
        counter++;
      }

    }

    /*--- Detect a wrong solution file ---*/

    if (iPoint_Global_Local < nPointDomain) { sbuf_NotMatching = 1; }

  #ifndef HAVE_MPI
    rbuf_NotMatching = sbuf_NotMatching;
  #else
    SU2_MPI::Allreduce(&sbuf_NotMatching, &rbuf_NotMatching, 1, MPI_UNSIGNED_SHORT, MPI_SUM, MPI_COMM_WORLD);
  #endif
    if (rbuf_NotMatching != 0) {
        SU2_MPI::Error(string("The solution file ") + filename_n1 + string(" doesn't match with the mesh file!\n") +
                       string("It could be empty lines at the end of the file."), CURRENT_FUNCTION);
    }

    /*--- Delete the class memory that is used to load the restart. ---*/

    if (Restart_Vars != NULL) delete [] Restart_Vars;
    if (Restart_Data != NULL) delete [] Restart_Data;
    Restart_Vars = NULL; Restart_Data = NULL;

  }

  /*--- It's necessary to communicate this information ---*/
  //geometry->Set_MPI_OldCoord(config);

  geometry->InitiateComms(geometry, config, COORDINATES_OLD);
  geometry->CompleteComms(geometry, config, COORDINATES_OLD);

}
