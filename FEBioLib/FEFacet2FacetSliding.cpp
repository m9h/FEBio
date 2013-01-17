#include "stdafx.h"
#include "FEFacet2FacetSliding.h"
#include "FECore/FEModel.h"
#include "FECore/log.h"

//-----------------------------------------------------------------------------
// Define sliding interface parameters
BEGIN_PARAMETER_LIST(FEFacet2FacetSliding, FEContactInterface)
	ADD_PARAMETER(m_epsn     , FE_PARAM_DOUBLE, "penalty"      );
	ADD_PARAMETER(m_bautopen , FE_PARAM_BOOL  , "auto_penalty" );
	ADD_PARAMETER(m_blaugon  , FE_PARAM_BOOL  , "laugon"       );
	ADD_PARAMETER(m_atol     , FE_PARAM_DOUBLE, "tolerance"    );
	ADD_PARAMETER(m_btwo_pass, FE_PARAM_BOOL  , "two_pass"     );
	ADD_PARAMETER(m_gtol     , FE_PARAM_DOUBLE, "gaptol"       );
	ADD_PARAMETER(m_naugmin  , FE_PARAM_INT   , "minaug"       );
	ADD_PARAMETER(m_naugmax  , FE_PARAM_INT   , "maxaug"       );
	ADD_PARAMETER(m_knmult   , FE_PARAM_DOUBLE, "knmult"       );
	ADD_PARAMETER(m_stol     , FE_PARAM_DOUBLE, "search_tol"   );
	ADD_PARAMETER(m_srad     , FE_PARAM_DOUBLE, "search_radius");
	ADD_PARAMETER(m_dxtol    , FE_PARAM_DOUBLE, "dxtol"        );
	ADD_PARAMETER(m_mu       , FE_PARAM_DOUBLE, "fric_coeff"   );
	ADD_PARAMETER(m_epsf     , FE_PARAM_DOUBLE, "fric_penalty" );
	ADD_PARAMETER(m_nsegup   , FE_PARAM_INT   , "seg_up"       );
END_PARAMETER_LIST();

//-----------------------------------------------------------------------------
// FEFacetSlidingSurface
//-----------------------------------------------------------------------------

bool FEFacetSlidingSurface::Init()
{
	// initialize surface data first
	if (FEContactSurface::Init() == false) return false;

	// count how many integration points we have
	int nint = 0, i;
	int NE = Elements();
	m_nei.resize(NE);
	for (i=0; i<NE; ++i)
	{
		FESurfaceElement& el = Element(i);
		m_nei[i] = nint;
		nint += el.GaussPoints();
	}

	// allocate data structures
	m_gap.assign(nint, 0.0);
	m_nu.resize(nint);
	m_rs.resize(nint);
	m_Lm.assign(nint, 0.0);
	m_pme.assign(nint, static_cast<FESurfaceElement*>(0));
	m_eps.assign(nint, 1.0);
	m_Ln.assign(nint, 0.0);

	// set intial values
	zero(m_nu);

	return true;
}

//-----------------------------------------------------------------------------
void FEFacetSlidingSurface::ShallowCopy(FEFacetSlidingSurface &s)
{
	m_Lm  = s.m_Lm;
	m_gap = s.m_gap;
	m_Ln = s.m_Ln;
	zero(m_pme);
}

//-----------------------------------------------------------------------------
void FEFacetSlidingSurface::Serialize(DumpFile& ar)
{
	FEContactSurface::Serialize(ar);
	if (ar.IsSaving())
	{
		ar << m_gap;
		ar << m_nu;
		ar << m_rs;
		ar << m_Lm;
		ar << m_nei;
		ar << m_eps;
		ar << m_Ln;
	}
	else
	{
		ar >> m_gap;
		ar >> m_nu;
		ar >> m_rs;
		ar >> m_Lm;
		ar >> m_nei;
		ar >> m_eps;
		ar >> m_Ln;
	}
}

//-----------------------------------------------------------------------------
// FEFacet2FacetSliding
//-----------------------------------------------------------------------------

FEFacet2FacetSliding::FEFacet2FacetSliding(FEModel* pfem) : FEContactInterface(pfem), m_ss(&pfem->GetMesh()), m_ms(&pfem->GetMesh())
{
	m_ntype = FE_FACET2FACET_SLIDING;
	static int ncount = 1;
	m_nID = ncount++;

	// default parameters
	m_epsn = 1.0;
	m_knmult = 1.0;
	m_stol = 0.01;
	m_btwo_pass = false;
	m_bautopen = false;
	m_nsegup = 0;	// always do segment updates

	m_atol = 0.01;
	m_gtol = 0;
	m_naugmin = 0;
	m_naugmax = 10;
	m_srad = 1.0;

	m_dxtol = 0;

	// Note that friction has not been implemented yet
	m_mu = 0;
	m_epsf = 0;

	m_ss.SetSibling(&m_ms);
	m_ms.SetSibling(&m_ss);
}

//-----------------------------------------------------------------------------
//! Initialization routine
bool FEFacet2FacetSliding::Init()
{
	// initialize surface data
	if (m_ss.Init() == false) return false;
	if (m_ms.Init() == false) return false;

	return true;
}

//-----------------------------------------------------------------------------
void FEFacet2FacetSliding::Activate()
{
	// don't forget the base class
	FEContactInterface::Activate();

	// calculate penalty factors
	if (m_bautopen) CalcAutoPenalty(m_ss);

	// project slave surface onto master surface
	ProjectSurface(m_ss, m_ms, true);

	if (m_btwo_pass) 
	{
		ProjectSurface(m_ms, m_ss, true);
		if (m_bautopen) CalcAutoPenalty(m_ms);
	}

	// check friction parameters
	// since friction has not been implemented yet
	if ((m_mu != 0) || (m_epsf != 0))
	{
		clog.printbox("WARNING", "Friction has NOT been implemented yet for facet-to-facet contact\ninterfaces. Friction parameters are ignored.");
		m_mu = 0;
		m_epsf = 0;
	}
}

//-----------------------------------------------------------------------------
void FEFacet2FacetSliding::CalcAutoPenalty(FEFacetSlidingSurface& s)
{
	// get the mesh
	FEMesh& m = m_pfem->GetMesh();

	// loop over all surface elements
	int ni = 0;
	for (int i=0; i<s.Elements(); ++i)
	{
		// get the surface element
		FESurfaceElement& el = s.Element(i);

		// find the element this face belongs to
		FEElement* pe = m.FindElementFromID(el.m_nelem);
		assert(pe);

		// get the area of the surface element
		double A = s.FaceArea(el);

		// get the volume of the volume element
		double V = m.ElementVolume(*pe);

		// calculate a modulus
		double K = BulkModulus(el, s);

		// calculate penalty
		double eps = K*A/V;

		// assign to integation points of surface element
		int nint = el.GaussPoints();
		for (int j=0; j<nint; ++j, ++ni) s.m_eps[ni] = eps;
	}
}

//-----------------------------------------------------------------------------
//! In this function we project the integration points to the master surface,
//! calculate the projection's natural coordinates and normal vector
//
void FEFacet2FacetSliding::ProjectSurface(FEFacetSlidingSurface &ss, FEFacetSlidingSurface &ms, bool bsegup)
{
	bool bfirst = true;

	// keep a running counter of which integration 
	// point we are currently investigating
	int ni = 0;

	// loop over all slave elements
	for (int i=0; i<ss.Elements(); ++i)
	{
		// get the slave element
		FESurfaceElement& se = ss.Element(i);
		int nn = se.Nodes();

		// get nodal coordinates
		vec3d re[FEElement::MAX_NODES];
		for (int l=0; l<nn; ++l) re[l] = ss.GetMesh()->Node(se.m_node[l]).m_rt;

		// loop over all its integration points
		int nint = se.GaussPoints();
		for (int j=0; j<nint; ++j, ++ni)
		{
			// calculate the global coordinates of this integration point
			double* H = se.H(j);

			vec3d x(0,0,0), q;
			for (int k=0; k<nn; ++k) x += re[k]*H[k];

			// see if the point still projects to the same element
			if (ss.m_pme[ni])
			{
				// update projection to master element
				FESurfaceElement& mel = *ss.m_pme[ni];
				q = ms.ProjectToSurface(mel, x, ss.m_rs[ni][0], ss.m_rs[ni][1]);

				// see if the projection is still in the element
				if (bsegup && (!ms.IsInsideElement(mel, ss.m_rs[ni][0], ss.m_rs[ni][1], m_stol)))
				{
					// if not, do a new search
					ss.m_rs[ni] = vec2d(0,0);
					FESurfaceElement* pme = ms.ClosestPointProjection(x, q, ss.m_rs[ni], bfirst, m_stol); bfirst = false;
					ss.m_pme[ni] = dynamic_cast<FESurfaceElement*>(pme);
				}
			}
			if (bsegup)
			{
				// find the master segment this element belongs to
				ss.m_rs[ni] = vec2d(0,0);
				FESurfaceElement* pme = ms.ClosestPointProjection(x, q, ss.m_rs[ni], bfirst, m_stol); bfirst = false;
				ss.m_pme[ni] = pme;
			}

			// update normal and gap at integration point
			if (ss.m_pme[ni])
			{
				double r = ss.m_rs[ni][0];
				double s = ss.m_rs[ni][1];

				// the slave normal is set to the master element normal
				ss.m_nu[ni] = ms.SurfaceNormal(*ss.m_pme[ni], r, s);

				// calculate gap
				ss.m_gap[ni] = -ss.m_nu[ni]*(x - q);
			}
			else
			{
				// since the node is not in contact, we set the gap and Lagrange multiplier to zero
				ss.m_gap[ni] = 0;
				ss.m_Lm[ni] = 0;
			}
		}
	}
}

//-----------------------------------------------------------------------------
void FEFacet2FacetSliding::Update(int niter)
{
	static bool bfirst = true;

	FEModel& fem = *m_pfem;

	// should we do a segment update or not?
	// TODO: check what happens when m_nsegup == -1 and m_npass = 2;
	// We have to make sure that in this case, both surfaces get at least
	// one pass!
	bool bupdate = (bfirst || (m_nsegup == 0)? true : (niter <= m_nsegup));

	// project slave surface to master surface
	ProjectSurface(m_ss, m_ms, bupdate);
	if (m_btwo_pass) ProjectSurface(m_ms, m_ss, bupdate);

	// Update the net contact pressures
	UpdateContactPressures();

	bfirst = false;
}

//-----------------------------------------------------------------------------
void FEFacet2FacetSliding::ShallowCopy(FEContactInterface &ci)
{
	FEFacet2FacetSliding& si = dynamic_cast<FEFacet2FacetSliding&>(ci);
	m_ss.ShallowCopy(si.m_ss);
	m_ms.ShallowCopy(si.m_ms);
}

//-----------------------------------------------------------------------------
void FEFacet2FacetSliding::ContactForces(FEGlobalVector& R)
{
	int i, j, k;
	vector<int> sLM, mLM, LM, en;
	vector<double> fe;

	// keep a running counter of integration points
	int ni = 0;

	const int MELN = FEElement::MAX_NODES;
	double detJ[MELN], w[MELN], *Hs, Hm[MELN];
	vec3d r0[MELN];

	int npass = (m_btwo_pass?2:1);
	for (int np=0; np<npass; ++np)
	{
		FEFacetSlidingSurface& ss = (np == 0? m_ss : m_ms);
		FEFacetSlidingSurface& ms = (np == 0? m_ms : m_ss);

		// loop over all slave elements
		ni = 0;
		for (i=0; i<ss.Elements(); ++i)
		{
			FESurfaceElement& se = ss.Element(i);
			int nseln = se.Nodes();
			int nint = se.GaussPoints();

			// get the element's LM vector
			ss.UnpackLM(se, sLM);

			// nodal coordinates
			for (j=0; j<nseln; ++j) r0[j] = ss.GetMesh()->Node(se.m_node[j]).m_r0;

			// we calculate all the metrics we need before we
			// calculate the nodal forces
			for (j=0; j<nint; ++j)
			{
				double* Gr = se.Gr(j);
				double* Gs = se.Gs(j);

				// calculate jacobian
				// note that we are integrating over the reference surface
				vec3d dxr, dxs;
				for (k=0; k<nseln; ++k)
				{
					dxr.x += Gr[k]*r0[k].x;
					dxr.y += Gr[k]*r0[k].y;
					dxr.z += Gr[k]*r0[k].z;

					dxs.x += Gs[k]*r0[k].x;
					dxs.y += Gs[k]*r0[k].y;
					dxs.z += Gs[k]*r0[k].z;
				}

				// jacobians
				detJ[j] = (dxr ^ dxs).norm();

				// integration weights
				w[j] = se.GaussWeights()[j];
			}

			// loop over all integration points
			for (int j=0; j<nint; ++j, ++ni)
			{
				// get the master element
				FESurfaceElement* pme = ss.m_pme[ni];
				if (pme)
				{
					FESurfaceElement& me = *pme;

					int nmeln = me.Nodes();
					ms.UnpackLM(me, mLM);

					// calculate degrees of freedom
					int ndof = 3*(nseln + nmeln);

					// build the LM vector
					LM.resize(ndof);
					for (k=0; k<nseln; ++k)
					{
						LM[3*k  ] = sLM[3*k  ];
						LM[3*k+1] = sLM[3*k+1];
						LM[3*k+2] = sLM[3*k+2];
					}

					for (k=0; k<nmeln; ++k)
					{
						LM[3*(k+nseln)  ] = mLM[3*k  ];
						LM[3*(k+nseln)+1] = mLM[3*k+1];
						LM[3*(k+nseln)+2] = mLM[3*k+2];
					}

					// build the en vector
					en.resize(nseln+nmeln);
					for (k=0; k<nseln; ++k) en[k] = se.m_node[k];
					for (k=0; k<nmeln; ++k) en[k+nseln] = me.m_node[k];

					// calculate shape functions
					Hs = se.H(j);

					double r = ss.m_rs[ni][0];
					double s = ss.m_rs[ni][1];
					me.shape_fnc(Hm, r, s);

					// get normal vector
					vec3d nu = ss.m_nu[ni];

					// gap function
					double g = ss.m_gap[ni];
					
					// lagrange multiplier
					double Lm = ss.m_Lm[ni];

					// penalty value
					double eps = m_epsn*ss.m_eps[ni];

					// contact traction
					double tn = Lm + eps*g;
					tn = MBRACKET(tn);

					// calculate the force vector
					fe.resize(ndof);

					for (k=0; k<nseln; ++k)
					{
						fe[3*k  ] = Hs[k]*nu.x;
						fe[3*k+1] = Hs[k]*nu.y;
						fe[3*k+2] = Hs[k]*nu.z;
					}

					for (k=0; k<nmeln; ++k)
					{
						fe[3*(k+nseln)  ] = -Hm[k]*nu.x;
						fe[3*(k+nseln)+1] = -Hm[k]*nu.y;
						fe[3*(k+nseln)+2] = -Hm[k]*nu.z;
					}

					for (k=0; k<ndof; ++k) fe[k] *= tn*detJ[j]*w[j];

					// assemble the global residual
					R.Assemble(en, LM, fe);
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------

void FEFacet2FacetSliding::ContactStiffness(FENLSolver* psolver)
{
	int i, j, k, l;
	vector<int> sLM, mLM, LM, en;
	const int MN = FEElement::MAX_NODES;
	const int ME = 3*MN*2;
	double N[ME], T1[ME], T2[ME], N1[ME] = {0}, N2[ME] = {0}, D1[ME], D2[ME], Nb1[ME], Nb2[ME];
	matrix ke;

	// keep a running counter of integration points
	int ni = 0;

	// get the mesh
	FEMesh* pm = m_ss.GetMesh();

	// see how many reformations we've had to do so far
	int nref = psolver->m_nref;

	// get the "size" of the model
	// We need this to scale the insertion distance
	double R = m_pfem->GetMesh().GetBoundingBox().radius();
	double dxtol = R*m_dxtol;

	// set higher order stiffness mutliplier
	double knmult = m_knmult;
	if (m_knmult < 0)
	{
		int ni = int(-m_knmult);
		if (nref >= ni)
		{
			knmult = 1; 
			clog.printf("Higher order stiffness terms included.\n");
		}
		else knmult = 0;
	}

	double detJ[MN], w[MN], *Hs, Hm[MN], Hmr[MN], Hms[MN];
	vec3d r0[MN];

	int npass = (m_btwo_pass?2:1);
	for (int np=0; np < npass; ++np)
	{
		FEFacetSlidingSurface& ss = (np == 0? m_ss : m_ms);
		FEFacetSlidingSurface& ms = (np == 0? m_ms : m_ss);

		// loop over all slave elements
		ni = 0;
		for (i=0; i<ss.Elements(); ++i)
		{
			FESurfaceElement& se = ss.Element(i);
			int nseln = se.Nodes();
			int nint = se.GaussPoints();

			// get the element's LM vector
			ss.UnpackLM(se, sLM);

			// nodal coordinates
			for (j=0; j<nseln; ++j) r0[j] = ss.GetMesh()->Node(se.m_node[j]).m_r0;

			// we calculate all the metrics we need before we
			// calculate the nodal forces
			for (j=0; j<nint; ++j)
			{
				double* Gr = se.Gr(j);
				double* Gs = se.Gs(j);

				// calculate jacobian
				// note that we are integrating over the reference surface
				vec3d dxr, dxs;
				for (k=0; k<nseln; ++k)
				{
					dxr.x += Gr[k]*r0[k].x;
					dxr.y += Gr[k]*r0[k].y;
					dxr.z += Gr[k]*r0[k].z;

					dxs.x += Gs[k]*r0[k].x;
					dxs.y += Gs[k]*r0[k].y;
					dxs.z += Gs[k]*r0[k].z;
				}

				// jacobians
				detJ[j] = (dxr ^ dxs).norm();

				// integration weights
				w[j] = se.GaussWeights()[j];
			}

			// loop over all integration points
			for (int j=0; j<nint; ++j, ++ni)
			{
				// get the master element
				FESurfaceElement* pme = ss.m_pme[ni];
				if (pme)
				{
					FESurfaceElement& me = *pme;

					int nmeln = me.Nodes();
					ms.UnpackLM(me, mLM);

					// calculate degrees of freedom
					int ndof = 3*(nseln + nmeln);

					// build the LM vector
					LM.resize(ndof);
					for (k=0; k<nseln; ++k)
					{
						LM[3*k  ] = sLM[3*k  ];
						LM[3*k+1] = sLM[3*k+1];
						LM[3*k+2] = sLM[3*k+2];
					}

					for (k=0; k<nmeln; ++k)
					{
						LM[3*(k+nseln)  ] = mLM[3*k  ];
						LM[3*(k+nseln)+1] = mLM[3*k+1];
						LM[3*(k+nseln)+2] = mLM[3*k+2];
					}

					// build the en vector
					en.resize(nseln+nmeln);
					for (k=0; k<nseln; ++k) en[k] = se.m_node[k];
					for (k=0; k<nmeln; ++k) en[k+nseln] = me.m_node[k];

					// calculate shape functions
					Hs = se.H(j);
					double r = ss.m_rs[ni][0];
					double s = ss.m_rs[ni][1];
					me.shape_fnc(Hm, r, s);

					// get normal vector
					vec3d nu = ss.m_nu[ni];

					// gap function
					double g = ss.m_gap[ni];
					
					// lagrange multiplier
					double Lm = ss.m_Lm[ni];

					// penalty value
					double eps = m_epsn*ss.m_eps[ni];

					// contact traction
					double tn = Lm + eps*g;
					tn = MBRACKET(tn);

					double dtn = eps*HEAVYSIDE(Lm + eps*g);

					// define buffer layer for penalty insertion
					if ((dtn < 1e-7) && (g < 0) && (dxtol != 0))
					{
						if (dxtol < 0) dtn = eps*exp(-g/dxtol);
						else if (-g<=dxtol) dtn = eps*(1 + g/dxtol);
					}

					// calculate the N-vector
					for (k=0; k<nseln; ++k)
					{
						N[3*k  ] = Hs[k]*nu.x;
						N[3*k+1] = Hs[k]*nu.y;
						N[3*k+2] = Hs[k]*nu.z;
					}

					for (k=0; k<nmeln; ++k)
					{
						N[3*(k+nseln)  ] = -Hm[k]*nu.x;
						N[3*(k+nseln)+1] = -Hm[k]*nu.y;
						N[3*(k+nseln)+2] = -Hm[k]*nu.z;
					}

					// --- N O R M A L   S T I F F N E S S ---

					// create the stiffness matrix
					ke.resize(ndof, ndof);

					// add the first order term (= D(tn)*dg )
					for (k=0; k<ndof; ++k)
						for (l=0; l<ndof; ++l) ke[k][l] = dtn*N[k]*N[l]*detJ[j]*w[j];

					// add the higher order terms (= tn*D(dg) )
					if (knmult > 0)
					{
						// calculate the master shape fncs derivatives
						me.shape_deriv(Hmr, Hms, r, s);

						// get the master nodes
						vec3d rt[MN];
						for (k=0; k<nmeln; ++k) rt[k] = ms.GetMesh()->Node(me.m_node[k]).m_rt;

						// get the tangent vectors
						vec3d tau1(0,0,0), tau2(0,0,0);
						for (k=0; k<nmeln; ++k)
						{
							tau1.x += Hmr[k]*rt[k].x;
							tau1.y += Hmr[k]*rt[k].y;
							tau1.z += Hmr[k]*rt[k].z;
		
							tau2.x += Hms[k]*rt[k].x;
							tau2.y += Hms[k]*rt[k].y;
							tau2.z += Hms[k]*rt[k].z;
						}

						// set up the Ti vectors
						for (k=0; k<nseln; ++k)
						{
							T1[k*3  ] = Hs[k]*tau1.x; T2[k*3  ] = Hs[k]*tau2.x;
							T1[k*3+1] = Hs[k]*tau1.y; T2[k*3+1] = Hs[k]*tau2.y;
							T1[k*3+2] = Hs[k]*tau1.z; T2[k*3+2] = Hs[k]*tau2.z;
						}

						for (k=0; k<nmeln; ++k) 
						{
							T1[(k+nseln)*3  ] = -Hm[k]*tau1.x;
							T1[(k+nseln)*3+1] = -Hm[k]*tau1.y;
							T1[(k+nseln)*3+2] = -Hm[k]*tau1.z;

							T2[(k+nseln)*3  ] = -Hm[k]*tau2.x;
							T2[(k+nseln)*3+1] = -Hm[k]*tau2.y;
							T2[(k+nseln)*3+2] = -Hm[k]*tau2.z;
						}

						// set up the Ni vectors
						for (k=0; k<nmeln; ++k) 
						{
							N1[(k+nseln)*3  ] = -Hmr[k]*nu.x;
							N1[(k+nseln)*3+1] = -Hmr[k]*nu.y;
							N1[(k+nseln)*3+2] = -Hmr[k]*nu.z;

							N2[(k+nseln)*3  ] = -Hms[k]*nu.x;
							N2[(k+nseln)*3+1] = -Hms[k]*nu.y;
							N2[(k+nseln)*3+2] = -Hms[k]*nu.z;
						}

						// calculate metric tensor
						mat2d M;
						M[0][0] = tau1*tau1; M[0][1] = tau1*tau2; 
						M[1][0] = tau2*tau1; M[1][1] = tau2*tau2; 

						// calculate reciprocal metric tensor
						mat2d Mi = M.inverse();

						// calculate curvature tensor
						double K[2][2] = {0};
						double Grr[MN];
						double Gss[MN];
						double Grs[MN];
						me.shape_deriv2(Grr, Grs, Gss, r, s);
						for (k=0; k<nmeln; ++k)
						{
							K[0][0] += (nu*rt[k])*Grr[k];
							K[0][1] += (nu*rt[k])*Grs[k];
							K[1][0] += (nu*rt[k])*Grs[k];
							K[1][1] += (nu*rt[k])*Gss[k];
						}

						// setup A matrix A = M + gK
						double A[2][2];
						A[0][0] = M[0][0] + g*K[0][0];
						A[0][1] = M[0][1] + g*K[0][1];
						A[1][0] = M[1][0] + g*K[1][0];
						A[1][1] = M[1][1] + g*K[1][1];

						// calculate determinant of A
						double detA = A[0][0]*A[1][1] - A[0][1]*A[1][0];

						// setup Di vectors
						for (k=0; k<ndof; ++k)
						{
							D1[k] = (1/detA)*(A[1][1]*(T1[k]+g*N1[k]) - A[0][1]*(T2[k] + g*N2[k]));
							D2[k] = (1/detA)*(A[0][0]*(T2[k]+g*N2[k]) - A[0][1]*(T1[k] + g*N1[k]));
						}

						// setup Nbi vectors
						for (k=0; k<ndof; ++k)
						{
							Nb1[k] = N1[k] - K[0][1]*D2[k];
							Nb2[k] = N2[k] - K[0][1]*D1[k];
						}

						// add it to the stiffness
						double sum;
						for (k=0; k<ndof; ++k)
							for (l=0; l<ndof; ++l)
							{
								sum = Mi[0][0]*Nb1[k]*Nb1[l]+Mi[0][1]*(Nb1[k]*Nb2[l]+Nb2[k]*Nb1[l])+Mi[1][1]*Nb2[k]*Nb2[l];
								sum *= g;
								sum -= D1[k]*N1[l]+D2[k]*N2[l]+N1[k]*D1[l]+N2[k]*D2[l];
								sum += K[0][1]*(D1[k]*D2[l]+D2[k]*D1[l]);
								sum *= tn*knmult;

								ke[k][l] += sum*detJ[j]*w[j];
							}
					}

					// assemble the global residual
					psolver->AssembleStiffness(en, LM, ke);
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
void FEFacet2FacetSliding::UpdateContactPressures()
{
	int np, n, i, j, k;
	int npass = (m_btwo_pass?2:1);
	for (np=0; np<npass; ++np)
	{
		FEFacetSlidingSurface& ss = (np == 0? m_ss : m_ms);
		FEFacetSlidingSurface& ms = (np == 0? m_ms : m_ss);
		
		// keep a running counter of integration points.
		int ni = 0;
		
		// loop over all elements of the primary surface
		for (n=0; n<ss.Elements(); ++n)
		{
			FESurfaceElement& el = ss.Element(n);
			int nint = el.GaussPoints();
			
			// get the normal tractions at the integration points
			double gap, eps;
			for (i=0; i<nint; ++i, ++ni) 
			{
				gap = ss.m_gap[ni];
				eps = m_epsn*ss.m_eps[ni];
				ss.m_Ln[ni] = MBRACKET(ss.m_Lm[ni] + eps*gap);
				FESurfaceElement* pme = ss.m_pme[ni];
				if (m_btwo_pass && pme)
				{
					int mint = pme->GaussPoints();
					int noff = ms.m_nei[pme->m_lid];
					double ti[FEElement::MAX_NODES];
					for (j=0; j<mint; ++j) {
						k = noff+j;
						gap = ms.m_gap[k];
						eps = m_epsn*ms.m_eps[k];
						ti[j] = MBRACKET(ms.m_Lm[k] + m_epsn*ms.m_eps[k]*ms.m_gap[k]);
					}
					// project the data to the nodes
					double tn[FEElement::MAX_NODES];
					pme->project_to_nodes(ti, tn);
					// now evaluate the traction at the intersection point
					double Ln = pme->eval(tn, ss.m_rs[ni][0], ss.m_rs[ni][1]);
					ss.m_Ln[ni] += MBRACKET(Ln);
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
bool FEFacet2FacetSliding::Augment(int naug)
{
	// make sure we need to augment
	if (!m_blaugon) return true;

	int i;
	double Ln;
	bool bconv = true;

	static double normg0 = 0;

	// --- c a l c u l a t e   i n i t i a l   n o r m s ---
	// a. normal component
	int NS = m_ss.m_Lm.size();
	int NM = m_ms.m_Lm.size();
	double normL0 = 0;
	for (i=0; i<NS; ++i) normL0 += m_ss.m_Lm[i]*m_ss.m_Lm[i];
	for (i=0; i<NM; ++i) normL0 += m_ms.m_Lm[i]*m_ms.m_Lm[i];
	normL0 = sqrt(normL0);

	// --- c a l c u l a t e   c u r r e n t   n o r m s ---
	// a. normal component
	double normL1 = 0;	// force norm
	double normg1 = 0;	// gap norm
	int N = 0;
	for (i=0; i<NS; ++i)
	{
		// penalty value
		double eps = m_epsn*m_ss.m_eps[i];

		// update Lagrange multipliers
		Ln = m_ss.m_Lm[i] + eps*m_ss.m_gap[i];
		Ln = MBRACKET(Ln);

		normL1 += Ln*Ln;

		if (m_ss.m_gap[i] > 0)
		{
			normg1 += m_ss.m_gap[i]*m_ss.m_gap[i];
			++N;
		}
	}	

	for (i=0; i<NM; ++i)
	{
		// penalty value
		double eps = m_epsn*m_ms.m_eps[i];

		// update Lagrange multipliers
		Ln = m_ms.m_Lm[i] + eps*m_ms.m_gap[i];
		Ln = MBRACKET(Ln);

		normL1 += Ln*Ln;
		if (m_ms.m_gap[i] > 0)
		{
			normg1 += m_ms.m_gap[i]*m_ms.m_gap[i];
			++N;
		}
	}
	if (N == 0) N=1;

	normL1 = sqrt(normL1);
	normg1 = sqrt(normg1 / N);

	if (naug == 0) normg0 = 0;

	// calculate and print convergence norms
	double lnorm = 0, gnorm = 0;
	if (normL1 != 0) lnorm = fabs(normL1 - normL0)/normL1; else lnorm = fabs(normL1 - normL0);
	if (normg1 != 0) gnorm = fabs(normg1 - normg0)/normg1; else gnorm = fabs(normg1 - normg0);

	clog.printf(" sliding interface # %d\n", m_nID);
	clog.printf("                        CURRENT        REQUIRED\n");
	clog.printf("    normal force : %15le", lnorm);
	if (m_atol > 0) clog.printf("%15le\n", m_atol); else clog.printf("       ***\n");
	clog.printf("    gap function : %15le", gnorm);
	if (m_gtol > 0) clog.printf("%15le\n", m_gtol); else clog.printf("       ***\n");

	// check convergence
	bconv = true;
	if ((m_atol > 0) && (lnorm > m_atol)) bconv = false;
	if ((m_gtol > 0) && (gnorm > m_gtol)) bconv = false;
	if (m_naugmin > naug) bconv = false;
	if (m_naugmax <= naug) bconv = true;
		
	if (bconv == false)
	{
		// we did not converge so update multipliers
		for (i=0; i<NS; ++i)
		{
			// penalty value
			double eps = m_epsn*m_ss.m_eps[i];

			// update Lagrange multipliers
			Ln = m_ss.m_Lm[i] + eps*m_ss.m_gap[i];
			m_ss.m_Lm[i] = MBRACKET(Ln);
		}	

		for (i=0; i<NM; ++i)
		{
			// penalty value
			double eps = m_epsn*m_ms.m_eps[i];

			// update Lagrange multipliers
			Ln = m_ms.m_Lm[i] + eps*m_ms.m_gap[i];
			m_ms.m_Lm[i] = MBRACKET(Ln);
		}
	}

	// store the last gap norm
	normg0 = normg1;

	return bconv;
}

//-----------------------------------------------------------------------------
void FEFacet2FacetSliding::Serialize(DumpFile &ar)
{
	// store contact data
	FEContactInterface::Serialize(ar);

	// store contact surface data
	m_ms.Serialize(ar);
	m_ss.Serialize(ar);
}
