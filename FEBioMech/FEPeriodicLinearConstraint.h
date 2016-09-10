#pragma once
#include <FECore/FEMesh.h>

class FEModel;

class FEPeriodicLinearConstraint
{
	struct NodeSetPair
	{
		FENodeSet master;
		FENodeSet slave;
	};

public:
	FEPeriodicLinearConstraint();
	~FEPeriodicLinearConstraint();

	void AddNodeSetPair(const FENodeSet& ms, const FENodeSet& ss, bool push_back = true);

	void SetReferenceNode(int node) { m_refNode = node; }

	bool GenerateConstraints(FEModel* fem);

	void ExcludeNodes(const FENodeSet& ps) { m_exclude = ps; }

private:
	vector<NodeSetPair>	m_set;	// list of node set pairs
	FENodeSet	m_exclude;		// nodes to exclude
	int			m_refNode;		// reference slave node
};
