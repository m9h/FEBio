/*This file is part of the FEBio source code and is licensed under the MIT license
listed below.

See Copyright-FEBio.txt for details.

Copyright (c) 2021 University of Utah, The Trustees of Columbia University in
the City of New York, and others.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.*/
#pragma once
#include <XML/XMLReader.h>

//-----------------------------------------------------------------------------
//! FEBio error terminated during the optimization
class FEErrorTermination{};

//-----------------------------------------------------------------------------
class FEOptimizeData;

//=============================================================================
//! Class that reads the optimization input file
class FEOptimizeInput
{
public:
	bool Input(const char* szfile, FEOptimizeData* pOpt);

private:
	void ParseTask(XMLTag& tag);
	void ParseOptions(XMLTag& tag);
	void ParseParameters(XMLTag& tag);
	void ParseConstraints(XMLTag& tag);
	void ParseObjective(XMLTag& tag);

	FEDataSource* ParseDataSource(XMLTag& tag);

private:
	void ParseObjectiveDataFit(XMLTag& tag);
	void ParseObjectiveTarget(XMLTag& tag);
	void ParseObjectiveElementData(XMLTag& tag);
	void ParseObjectiveNodeData(XMLTag& tag);

private:
	FEOptimizeData*	m_opt;
};
