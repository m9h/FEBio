#include "stdafx.h"
#include "FEFluidDomain3D.h"
#include "FECore/log.h"
#include "FECore/DOFS.h"
#include "NumCore/LUSolver.h"

#ifdef WIN32
extern "C" int __cdecl omp_get_num_threads(void);
extern "C" int __cdecl omp_get_thread_num(void);
#else
extern "C" int omp_get_num_threads(void);
extern "C" int omp_get_thread_num(void);
#endif


//-----------------------------------------------------------------------------
//! constructor
//! Some derived classes will pass 0 to the pmat, since the pmat variable will be
//! to initialize another material. These derived classes will set the m_pMat variable as well.
FEFluidDomain3D::FEFluidDomain3D(FEModel* pfem) : FESolidDomain(&pfem->GetMesh()), FEFluidDomain(pfem)
{
    m_pMat = 0;
    m_btrans = true;
    
    m_dofVX = pfem->GetDOFIndex("vx");
    m_dofVY = pfem->GetDOFIndex("vy");
    m_dofVZ = pfem->GetDOFIndex("vz");
    m_dofE  = pfem->GetDOFIndex("e");

	// list the degrees of freedom
	// (This allows the FEBomain base class to handle several tasks such as UnpackLM)
	vector<int> dof;
	dof.push_back(m_dofVX);
	dof.push_back(m_dofVY);
	dof.push_back(m_dofVZ);
	dof.push_back(m_dofE);
	SetDOF(dof);
}

//-----------------------------------------------------------------------------
// \todo I don't think this is being used
FEFluidDomain3D& FEFluidDomain3D::operator = (FEFluidDomain3D& d)
{
    m_Elem = d.m_Elem;
    m_pMesh = d.m_pMesh;
    return (*this);
}

//-----------------------------------------------------------------------------
//! Assign material
void FEFluidDomain3D::SetMaterial(FEMaterial* pmat)
{
    if (pmat)
    {
        m_pMat = dynamic_cast<FEFluid*>(pmat);
        assert(m_pMat);
    }
    else m_pMat = 0;
}

//-----------------------------------------------------------------------------
//! \todo The material point initialization needs to move to the base class.
bool FEFluidDomain3D::Initialize(FEModel &fem)
{
    // initialize base class
    FESolidDomain::Initialize(fem);
    
    // get the elements material
    FEFluid* pme = m_pMat;
    
    // assign local coordinate system to each integration point
    for (size_t i=0; i<m_Elem.size(); ++i)
    {
        FESolidElement& el = m_Elem[i];
        for (int n=0; n<el.GaussPoints(); ++n) pme->SetLocalCoordinateSystem(el, n, *(el.GetMaterialPoint(n)));
    }
    
    // check for initially inverted elements
    int ninverted = 0;
    for (int i=0; i<Elements(); ++i)
    {
        FESolidElement& el = Element(i);
        
        int nint = el.GaussPoints();
        for (int n=0; n<nint; ++n)
        {
            double J0 = detJ0(el, n);
            if (J0 <= 0)
            {
                felog.printf("**************************** E R R O R ****************************\n");
                felog.printf("Negative jacobian detected at integration point %d of element %d\n", n+1, el.GetID());
                felog.printf("Jacobian = %lg\n", J0);
                felog.printf("Did you use the right node numbering?\n");
                felog.printf("Nodes:");
                for (int l=0; l<el.Nodes(); ++l)
                {
                    felog.printf("%d", el.m_node[l]+1);
                    if (l+1 != el.Nodes()) felog.printf(","); else felog.printf("\n");
                }
                felog.printf("*******************************************************************\n\n");
                ++ninverted;
            }
        }
    }
    
    return (ninverted == 0);
}

//-----------------------------------------------------------------------------
//! Initialize element data
void FEFluidDomain3D::InitElements()
{
    const int NE = FEElement::MAX_NODES;
    vec3d x0[NE], vp[NE], r0, v;
    FEMesh& m = *GetMesh();
    for (size_t i=0; i<m_Elem.size(); ++i)
    {
        FESolidElement& el = m_Elem[i];
        int neln = el.Nodes();
        for (int i=0; i<neln; ++i)
        {
            x0[i] = m.Node(el.m_node[i]).m_r0;
            vp[i] = m.Node(el.m_node[i]).m_vp;
        }
        
        int n = el.GaussPoints();
        for (int j=0; j<n; ++j)
        {
            FEMaterialPoint& mp = *el.GetMaterialPoint(j);
            FEFluidMaterialPoint& pt = *mp.ExtractData<FEFluidMaterialPoint>();
            pt.m_r0 = el.Evaluate(x0, j);
            pt.m_vp = el.Evaluate(vp, j);
            pt.m_Jp = pt.m_J;
            
            if (pt.m_J <= 0) {
                felog.printbox("ERROR", "Negative jacobian was detected.");
                throw DoRunningRestart();
            }
            
            mp.Init(false);
        }
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::InternalForces(FEGlobalVector& R)
{
    int NE = (int)m_Elem.size();
#pragma omp parallel for shared (NE)
    for (int i=0; i<NE; ++i)
    {
        // element force vector
        vector<double> fe;
        vector<int> lm;
        
        // get the element
        FESolidElement& el = m_Elem[i];
        
        // get the element force vector and initialize it to zero
        int ndof = 4*el.Nodes();
        fe.assign(ndof, 0);
        
        // calculate internal force vector
        ElementInternalForce(el, fe);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble element 'fe'-vector into global R vector
        //#pragma omp critical
        R.Assemble(el.m_node, lm, fe);
    }
}

//-----------------------------------------------------------------------------
//! calculates the internal equivalent nodal forces for solid elements

void FEFluidDomain3D::ElementInternalForce(FESolidElement& el, vector<double>& fe)
{
    int i, n;
    
    // jacobian matrix, inverse jacobian matrix and determinants
    double Ji[3][3], detJ;
    
    mat3ds s;
    
    const double *H, *Gr, *Gs, *Gt;
    
    int nint = el.GaussPoints();
    int neln = el.Nodes();
    
    bool bsupg = m_pMat->m_bsupg;
    
    double tau, tau1, tau2, tau3;
    
    // gradient of shape functions
    vector<vec3d> gradN(neln);
    
    double*	gw = el.GaussWeights();
    
    // repeat for all integration points
    for (n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        FEFluidMaterialPoint& pt = *(mp.ExtractData<FEFluidMaterialPoint>());
        
        // calculate the jacobian
        detJ = invjac0(el, Ji, n)*gw[n];
        
        vec3d g1(Ji[0][0],Ji[0][1],Ji[0][2]);
        vec3d g2(Ji[1][0],Ji[1][1],Ji[1][2]);
        vec3d g3(Ji[2][0],Ji[2][1],Ji[2][2]);

        // get the stress tensor for this integration point
        s = pt.m_s;
        
        H = el.H(n);
        Gr = el.Gr(n);
        Gs = el.Gs(n);
        Gt = el.Gt(n);
        
        // evaluate spatial gradient of shape functions
        tau = tau1 = tau2 = tau3 = 0;
        double c = m_pMat->AcousticSpeed(mp);
        double nu = m_pMat->KinematicViscosity(mp);
        for (i=0; i<neln; ++i)
        {
            gradN[i] = g1*Gr[i] + g2*Gs[i] + g3*Gt[i];
            vec3d j = pt.m_gradJ; j.unit();
            vec3d vu = pt.m_vt; vu.unit();
            vec3d r = pt.m_L.transpose()*vu; r.unit();
            tau1 += c*fabs(j*gradN[i]) + fabs(pt.m_vt*gradN[i]);
            tau3 += fabs(r*gradN[i]);
        }
        tau2 = 2./mp.dt;
        tau3 = nu*tau3*tau3;
        tau = tau1*tau1 + m_btrans*tau2*tau2 + tau3*tau3;
        if (tau > 0) tau = pow(tau,-0.5);
        
        // this is the governing equation for v
        double dpdJ = m_pMat->GetElastic()->Tangent_Pressure_Strain(mp);
        double mu = m_pMat->GetViscous()->DynamicViscosity(mp);
        double rho = m_pMat->Density(mp);
        // TODO: add bulk viscosity term
        vec3d divT = pt.m_gradJ*(-dpdJ) + pt.m_gdiv*(mu/3) + pt.m_lapv*mu;
        vec3d fv = divT - pt.m_at*rho;
        
        // this is the governing equation for J
        double divv = pt.m_L.trace();
        double f = ((pt.m_J - pt.m_Jp)/mp.dt)*m_btrans + pt.m_gradJ*pt.m_vt - pt.m_J*divv;
        
        for (i=0; i<neln; ++i)
        {
            vec3d fs = s*gradN[i];
            double fJ = f*H[i];
            if (bsupg)
            {
                fs += fv*(tau*(gradN[i]*pt.m_vt));
                fJ += tau*f*(gradN[i]*pt.m_vt);
            }
            // calculate internal force
            // the '-' sign is so that the internal forces get subtracted
            // from the global residual vector
            fe[4*i  ] -= fs.x*detJ;
            fe[4*i+1] -= fs.y*detJ;
            fe[4*i+2] -= fs.z*detJ;
            fe[4*i+3] -= fJ*detJ;
        }
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::BodyForce(FEGlobalVector& R, FEBodyForce& BF)
{
    int NE = (int)m_Elem.size();
#pragma omp parallel for
    for (int i=0; i<NE; ++i)
    {
        vector<double> fe;
        vector<int> lm;
        
        // get the element
        FESolidElement& el = m_Elem[i];
        
        // get the element force vector and initialize it to zero
        int ndof = 4*el.Nodes();
        fe.assign(ndof, 0);
        
        // apply body forces
        ElementBodyForce(BF, el, fe);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble element 'fe'-vector into global R vector
        R.Assemble(el.m_node, lm, fe);
    }
}

//-----------------------------------------------------------------------------
//! calculates the body forces

void FEFluidDomain3D::ElementBodyForce(FEBodyForce& BF, FESolidElement& el, vector<double>& fe)
{
    // jacobian
    double detJ;
    double *H;
    double* gw = el.GaussWeights();
    vec3d f;
    
    // number of nodes
    int neln = el.Nodes();
    
    // nodal coordinates
    vec3d r0[FEElement::MAX_NODES];
    for (int i=0; i<neln; ++i)
        r0[i] = m_pMesh->Node(el.m_node[i]).m_r0;
    
    // loop over integration points
    int nint = el.GaussPoints();
    for (int n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        FEFluidMaterialPoint& pt = *mp.ExtractData<FEFluidMaterialPoint>();
        double dens = m_pMat->Density(mp);
        
        pt.m_r0 = el.Evaluate(r0, n);
        
        detJ = detJ0(el, n)*gw[n];
        
        // get the force
        f = BF.force(mp);
        
        H = el.H(n);
        
        for (int i=0; i<neln; ++i)
        {
            fe[4*i  ] -= H[i]*dens*f.x*detJ;
            fe[4*i+1] -= H[i]*dens*f.y*detJ;
            fe[4*i+2] -= H[i]*dens*f.z*detJ;
        }
    }
}

//-----------------------------------------------------------------------------
//! This function calculates the stiffness due to body forces
void FEFluidDomain3D::ElementBodyForceStiffness(FEBodyForce& BF, FESolidElement &el, matrix &ke)
{
    int neln = el.Nodes();
    int ndof = ke.columns()/neln;
    
    // jacobian
    double detJ;
    double *H;
    double* gw = el.GaussWeights();
    vec3d f, k;
    
    // gradient of shape functions
    vec3d gradN;
    
    // loop over integration points
    int nint = el.GaussPoints();
    for (int n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        FEFluidMaterialPoint& pt = *mp.ExtractData<FEFluidMaterialPoint>();

        // calculate the jacobian
        detJ = detJ0(el, n)*gw[n];
        
        H = el.H(n);
        
        double dens = m_pMat->Density(mp);
        
        // get the force
        f = BF.force(mp);
        
        H = el.H(n);
        
        for (int i=0; i<neln; ++i) {
            for (int j=0; j<neln; ++j)
            {
                k = f*(-H[i]*H[j]*dens/pt.m_J*detJ);
                ke[ndof*i  ][ndof*j+3] += k.x;
                ke[ndof*i+1][ndof*j+3] += k.y;
                ke[ndof*i+2][ndof*j+3] += k.z;
            }
        }
    }
}

//-----------------------------------------------------------------------------
//! Calculates element material stiffness element matrix

void FEFluidDomain3D::ElementMaterialStiffness(FESolidElement &el, matrix &ke)
{
    int i, i4, j, j4, n;
    
    // Get the current element's data
    const int nint = el.GaussPoints();
    const int neln = el.Nodes();
    
    bool bsupg = m_pMat->m_bsupg;
    
    double tau, tau1, tau2, tau3;
    
    // gradient of shape functions
    vector<vec3d> gradN(neln);
    
    double *H, *Gr, *Gs, *Gt;
    
    // jacobian
    double Ji[3][3], detJ;
    
    // weights at gauss points
    const double *gw = el.GaussWeights();
    
    // calculate element stiffness matrix
    for (n=0; n<nint; ++n)
    {
        // calculate jacobian
        detJ = invjac0(el, Ji, n)*gw[n];
        
        vec3d g1(Ji[0][0],Ji[0][1],Ji[0][2]);
        vec3d g2(Ji[1][0],Ji[1][1],Ji[1][2]);
        vec3d g3(Ji[2][0],Ji[2][1],Ji[2][2]);
        
        H = el.H(n);
        Gr = el.Gr(n);
        Gs = el.Gs(n);
        Gt = el.Gt(n);
        
        // setup the material point
        // NOTE: deformation gradient and determinant have already been evaluated in the stress routine
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        FEFluidMaterialPoint& pt = *(mp.ExtractData<FEFluidMaterialPoint>());
        
        // get the tangents
        mat3ds sJ = m_pMat->Tangent_Strain(mp);
        tens4ds cv = m_pMat->Tangent_RateOfDeformation(mp);
        
        // evaluate spatial gradient of shape functions
        vec3d g(0,0,0);
        tau = tau1 = tau2 = tau3 = 0;
        double c = m_pMat->AcousticSpeed(mp);
        double nu = m_pMat->KinematicViscosity(mp);
        for (i=0; i<neln; ++i)
        {
            gradN[i] = g1*Gr[i] + g2*Gs[i] + g3*Gt[i];
            vec3d j = pt.m_gradJ; j.unit();
            vec3d vu = pt.m_vt; vu.unit();
            vec3d r = pt.m_L.transpose()*vu; r.unit();
            tau1 += c*fabs(j*gradN[i]) + fabs(pt.m_vt*gradN[i]);
            tau3 += fabs(r*gradN[i]);
            double d = pt.m_vt*gradN[i];
            if (d > 0) g += gradN[i];
            else if (d < 0) g -= gradN[i];
        }
        g /= neln;
        tau2 = 2./mp.dt;
        tau3 = nu*tau3*tau3;
        tau = tau1*tau1 + m_btrans*tau2*tau2 + tau3*tau3;
        if (tau > 0) tau = pow(tau,-0.5);
        
        // this is the governing equation for v
        double dpdJ = m_pMat->GetElastic()->Tangent_Pressure_Strain(mp);
        double mu = m_pMat->GetViscous()->DynamicViscosity(mp);
        double rho = m_pMat->Density(mp);
        mat3ds TpJ = m_pMat->Tangent_Strain(mp);
        // TODO: add bulk viscosity term
        vec3d divT = pt.m_gradJ*(-dpdJ) + pt.m_gdiv*(mu/3) + pt.m_lapv*mu;
        vec3d fv = divT - pt.m_at*rho;
        
        // this is the governing equation for J
        double divv = pt.m_L.trace();
        double f = ((pt.m_J - pt.m_Jp)/mp.dt)*m_btrans + pt.m_gradJ*pt.m_vt - pt.m_J*divv;
        
        // evaluate stiffness matrix
        for (i=0, i4=0; i<neln; ++i, i4 += 4)
        {
            for (j=0, j4 = 0; j<neln; ++j, j4 += 4)
            {
                mat3d Kvv = vdotTdotv(gradN[i], cv, gradN[j])*detJ;
                vec3d kJv = (pt.m_gradJ*H[j] - gradN[j]*pt.m_J)*(H[i]*detJ);
                vec3d kvJ = (sJ*gradN[i])*(H[j]*detJ);
                double kJJ = (H[j]*((1.0*m_btrans)/mp.dt - divv) + gradN[j]*pt.m_vt)*(H[i]*detJ);
                
                if (bsupg) {
                    Kvv += (fv & g)*(gradN[i]*pt.m_vt)*(-tau*tau*H[j]*detJ)
                    + (fv & gradN[i])*(H[j]*tau*detJ)
                    - (mat3dd(H[j]/mp.dt*m_btrans+gradN[j]*pt.m_vt)+pt.m_L*H[j])*(pt.m_vt*gradN[i])*(rho*tau*detJ);
                    kvJ += (TpJ*gradN[j] + pt.m_at*(H[j]*rho/pt.m_J))*(gradN[i]*pt.m_vt)*(tau*detJ);
                    kJv += g*(-(gradN[i]*pt.m_vt)*tau*tau*f*H[j]*detJ)
                    + gradN[i]*(tau*f*H[j]*detJ)
                    + (pt.m_gradJ*H[j]-gradN[j]*pt.m_J)*(gradN[i]*pt.m_vt)*(tau*detJ);
                    kJJ += tau*(gradN[i]*pt.m_vt)*((1/mp.dt*m_btrans-divv)*H[j]+gradN[j]*pt.m_vt)*detJ;
                }
                
                ke[i4  ][j4  ] += Kvv(0,0);
                ke[i4  ][j4+1] += Kvv(0,1);
                ke[i4  ][j4+2] += Kvv(0,2);
                ke[i4  ][j4+3] += kvJ.x;
                
                ke[i4+1][j4  ] += Kvv(1,0);
                ke[i4+1][j4+1] += Kvv(1,1);
                ke[i4+1][j4+2] += Kvv(1,2);
                ke[i4+1][j4+3] += kvJ.y;
                
                ke[i4+2][j4  ] += Kvv(2,0);
                ke[i4+2][j4+1] += Kvv(2,1);
                ke[i4+2][j4+2] += Kvv(2,2);
                ke[i4+2][j4+3] += kvJ.z;
                
                ke[i4+3][j4  ] += kJv.x;
                ke[i4+3][j4+1] += kJv.y;
                ke[i4+3][j4+2] += kJv.z;
                ke[i4+3][j4+3] += kJJ;
            }
        }
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::StiffnessMatrix(FESolver* psolver)
{
    // repeat over all solid elements
    int NE = (int)m_Elem.size();
    
#pragma omp parallel for shared (NE)
    for (int iel=0; iel<NE; ++iel)
    {
        // element stiffness matrix
        matrix ke;
        vector<int> lm;
        
        FESolidElement& el = m_Elem[iel];
        
        // create the element's stiffness matrix
        int ndof = 4*el.Nodes();
        ke.resize(ndof, ndof);
        ke.zero();
        
        // calculate material stiffness
        ElementMaterialStiffness(el, ke);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble element matrix in global stiffness matrix
#pragma omp critical
        psolver->AssembleStiffness(el.m_node, lm, ke);
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::MassMatrix(FESolver* psolver)
{
    // repeat over all solid elements
    int NE = (int)m_Elem.size();
    
#pragma omp parallel for shared (NE)
    for (int iel=0; iel<NE; ++iel)
    {
        // element stiffness matrix
        matrix ke;
        vector<int> lm;
        
        FESolidElement& el = m_Elem[iel];
        
        // create the element's stiffness matrix
        int ndof = 4*el.Nodes();
        ke.resize(ndof, ndof);
        ke.zero();
        
        // calculate inertial stiffness
        ElementMassMatrix(el, ke);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble element matrix in global stiffness matrix
#pragma omp critical
        psolver->AssembleStiffness(el.m_node, lm, ke);
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::BodyForceStiffness(FESolver* psolver, FEBodyForce& bf)
{
    FEFluid* pme = dynamic_cast<FEFluid*>(GetMaterial()); assert(pme);
    
    // repeat over all solid elements
    int NE = (int)m_Elem.size();
    
#pragma omp parallel for shared (NE)
    for (int iel=0; iel<NE; ++iel)
    {
        // element stiffness matrix
        matrix ke;
        vector<int> lm;
        
        FESolidElement& el = m_Elem[iel];
        
        // create the element's stiffness matrix
        int ndof = 4*el.Nodes();
        ke.resize(ndof, ndof);
        ke.zero();
        
        // calculate inertial stiffness
        ElementBodyForceStiffness(bf, el, ke);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble element matrix in global stiffness matrix
#pragma omp critical
        psolver->AssembleStiffness(el.m_node, lm, ke);
    }
}

//-----------------------------------------------------------------------------
//! This function calculates the element stiffness matrix. It calls the material
//! stiffness function

void FEFluidDomain3D::ElementStiffness(FEModel& fem, int iel, matrix& ke)
{
    FESolidElement& el = Element(iel);
    
    // calculate material stiffness (i.e. constitutive component)
    ElementMaterialStiffness(el, ke);
    
}

//-----------------------------------------------------------------------------
//! calculates element inertial stiffness matrix
void FEFluidDomain3D::ElementMassMatrix(FESolidElement& el, matrix& ke)
{
    int i, i4, j, j4, n;
    
    // Get the current element's data
    const int nint = el.GaussPoints();
    const int neln = el.Nodes();
    
    // gradient of shape functions
    vector<vec3d> gradN(neln);
    
    double *H;
    double *Gr, *Gs, *Gt;
    
    // jacobian
    double Ji[3][3], detJ;
    
    // weights at gauss points
    const double *gw = el.GaussWeights();
    
    // calculate element stiffness matrix
    for (n=0; n<nint; ++n)
    {
        // calculate jacobian
        detJ = invjac0(el, Ji, n)*gw[n];
        
        vec3d g1(Ji[0][0],Ji[0][1],Ji[0][2]);
        vec3d g2(Ji[1][0],Ji[1][1],Ji[1][2]);
        vec3d g3(Ji[2][0],Ji[2][1],Ji[2][2]);
        
        H = el.H(n);
        
        Gr = el.Gr(n);
        Gs = el.Gs(n);
        Gt = el.Gt(n);
        
        // setup the material point
        // NOTE: deformation gradient and determinant have already been evaluated in the stress routine
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        FEFluidMaterialPoint& pt = *(mp.ExtractData<FEFluidMaterialPoint>());
        
        double dt = mp.dt;
        double dens = m_pMat->Density(mp);
        
        // evaluate spatial gradient of shape functions
        for (i=0; i<neln; ++i)
            gradN[i] = g1*Gr[i] + g2*Gs[i] + g3*Gt[i];
        
        // evaluate stiffness matrix
        for (i=0, i4=0; i<neln; ++i, i4 += 4)
        {
            for (j=0, j4 = 0; j<neln; ++j, j4 += 4)
            {
                mat3d Mv = ((mat3dd(1)*(m_btrans/dt) + pt.m_L)*H[j] + mat3dd(gradN[j]*pt.m_vt))*(H[i]*dens*detJ);
                vec3d mJ = pt.m_at*(-H[i]*H[j]*dens/pt.m_J*detJ);
                
                ke[i4  ][j4  ] += Mv(0,0);
                ke[i4  ][j4+1] += Mv(0,1);
                ke[i4  ][j4+2] += Mv(0,2);
                ke[i4  ][j4+3] += mJ.x;
                
                ke[i4+1][j4  ] += Mv(1,0);
                ke[i4+1][j4+1] += Mv(1,1);
                ke[i4+1][j4+2] += Mv(1,2);
                ke[i4+1][j4+3] += mJ.y;
                
                ke[i4+2][j4  ] += Mv(2,0);
                ke[i4+2][j4+1] += Mv(2,1);
                ke[i4+2][j4+2] += Mv(2,2);
                ke[i4+2][j4+2] += mJ.z;
            }
        }
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::Update()
{
	FEModel& fem = *GetFEModel();
    double dt = fem.GetCurrentStep()->m_dt;
    
    // TODO: This is temporary hack for running micro-materials in parallel.
    //	     Evaluating the stress for a micro-material will make FEBio solve
    //       a new FE problem. We don't want to see the output of that problem.
    //       The logfile is a shared resource between the master FEM and the RVE
    //       in order not to corrupt the logfile we don't print anything for
    //       the RVE problem.
    // TODO: Maybe I need to create a new domain class for micro-material.
    Logfile::MODE nmode = felog.GetMode();
    felog.SetMode(Logfile::NEVER);
    
    bool berr = false;
    int NE = (int) m_Elem.size();
#pragma omp parallel for shared(NE, berr)
    for (int i=0; i<NE; ++i)
    {
        try
        {
            UpdateElementStress(i, dt);
        }
        catch (NegativeJacobian e)
        {
#pragma omp critical
            {
                // reset the logfile mode
                felog.SetMode(nmode);
                berr = true;
                if (NegativeJacobian::m_boutput) e.print();
            }
        }
    }
    
    // reset the logfile mode
    felog.SetMode(nmode);
    
    // if we encountered an error, we request a running restart
    if (berr)
    {
        if (NegativeJacobian::m_boutput == false) felog.printbox("ERROR", "Negative jacobian was detected.");
        throw DoRunningRestart();
    }
}

//-----------------------------------------------------------------------------
//! Update element state data (mostly stresses, but some other stuff as well)
void FEFluidDomain3D::UpdateElementStress(int iel, double dt)
{
    // get the solid element
    FESolidElement& el = m_Elem[iel];
    
    // get the number of integration points
    int nint = el.GaussPoints();
    
    // number of nodes
    int neln = el.Nodes();
    
    bool bsupg = m_pMat->m_bsupg;
    
    // nodal coordinates
    vec3d vt[FEElement::MAX_NODES];
    double et[FEElement::MAX_NODES];
    for (int j=0; j<neln; ++j) {
        vt[j] = m_pMesh->Node(el.m_node[j]).get_vec3d(m_dofVX, m_dofVY, m_dofVZ);
        et[j] = m_pMesh->Node(el.m_node[j]).get(m_dofE);
    }
    
    // loop over the integration points and update
    // velocity, velocity gradient, acceleration
    // stress and pressure at the integration point
    for (int n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        FEFluidMaterialPoint& pt = *(mp.ExtractData<FEFluidMaterialPoint>());
        
        // material point data
        pt.m_vt = el.Evaluate(vt, n);
        pt.m_L = gradient(el, vt, n);
        pt.m_at = ((pt.m_vt - pt.m_vp)/dt)*m_btrans + pt.m_L*pt.m_vt;
        pt.m_J = 1 + el.Evaluate(et, n);
        pt.m_gradJ = gradient(el, et, n);

        // calculate the stress at this material point
        pt.m_s = m_pMat->Stress(mp);
        
        // calculate the fluid pressure
        pt.m_p = m_pMat->GetElastic()->Pressure(mp);
        
        // for SUPG stabilization calculate higher order velocity derivatives
        if (bsupg) {
            pt.m_lapv = lapvec(el, vt, n);
            pt.m_gdiv = gradivec(el, vt, n);
        }
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::InertialForces(FEGlobalVector& R)
{
    int NE = (int)m_Elem.size();
#pragma omp parallel for shared (NE)
    for (int i=0; i<NE; ++i)
    {
        // element force vector
        vector<double> fe;
        vector<int> lm;
        
        // get the element
        FESolidElement& el = m_Elem[i];
        
        // get the element force vector and initialize it to zero
        int ndof = 4*el.Nodes();
        fe.assign(ndof, 0);
        
        // calculate internal force vector
        ElementInertialForce(el, fe);
        
        // get the element's LM vector
        UnpackLM(el, lm);
        
        // assemble element 'fe'-vector into global R vector
        //#pragma omp critical
        R.Assemble(el.m_node, lm, fe);
    }
}

//-----------------------------------------------------------------------------
void FEFluidDomain3D::ElementInertialForce(FESolidElement& el, vector<double>& fe)
{
    int i, n;
    
    // jacobian determinant
    double detJ;
    
    const double* H;
    
    int nint = el.GaussPoints();
    int neln = el.Nodes();
    
    double*	gw = el.GaussWeights();
    
    // repeat for all integration points
    for (n=0; n<nint; ++n)
    {
        FEMaterialPoint& mp = *el.GetMaterialPoint(n);
        FEFluidMaterialPoint& pt = *(mp.ExtractData<FEFluidMaterialPoint>());
        double dens = m_pMat->Density(mp);
        
        // calculate the jacobian
        detJ = detJ0(el, n)*gw[n];
        
        H = el.H(n);
        
        for (i=0; i<neln; ++i)
        {
            vec3d f = pt.m_at*(dens*H[i]);
            
            // calculate internal force
            // the '-' sign is so that the internal forces get subtracted
            // from the global residual vector
            fe[4*i  ] -= f.x*detJ;
            fe[4*i+1] -= f.y*detJ;
            fe[4*i+2] -= f.z*detJ;
        }
    }
}
