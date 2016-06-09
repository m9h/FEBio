#pragma once
#include "FEBioImport.h"

//-----------------------------------------------------------------------------
// Geometry Section
class FEBioGeometrySection : public FEBioFileSection
{
private:
	struct FEDOMAIN 
	{
		FE_Element_Spec		elem;	// element type
		int					mat;	// material ID
		int					nel;	// number of elements
	};
	
public:
	FEBioGeometrySection(FEBioImport* pim) : FEBioFileSection(pim){}
	void Parse(XMLTag& tag);

protected:
	void ParseNodeSection       (XMLTag& tag);
	void ParseElementSection    (XMLTag& tag);
	void ParseElementSection20  (XMLTag& tag);
	void ParseElementDataSection(XMLTag& tag);
	void ParseNodeSetSection    (XMLTag& tag);
	void ParseDiscreteSetSection(XMLTag& tag);
	void ParseEdgeSection       (XMLTag& tag);
	void ParseSurfaceSection    (XMLTag& tag);
	void ParseSurfacePairSection(XMLTag& tag);
	void ParseElementSetSection (XMLTag& tag);
	void ParseElementData(FEElement& el, XMLTag& tag);

	void ParseMesh(XMLTag& tag);

	void ReadElement(XMLTag& tag, FEElement& el, int nid, int nmat);

	FE_Element_Spec ElementSpec(const char* sz);
	FEDomain* CreateDomain(const FE_Element_Spec& spec, FEMesh* pm, FEMaterial* pmat);

protected:
	vector<FEDOMAIN>	m_dom;
};
