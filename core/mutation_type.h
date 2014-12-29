//
//  mutation_type.h
//  SLiM
//
//  Created by Ben Haller on 12/13/14.
//  Copyright (c) 2014 Philipp Messer.  All rights reserved.
//	A product of the Messer Lab, http://messerlab.org/software/
//

//	This file is part of SLiM.
//
//	SLiM is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
//
//	SLiM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License along with SLiM.  If not, see <http://www.gnu.org/licenses/>.

/*
 
 The class MutationType represents a type of mutation defined in the input file, such as a synonymous mutation or an adaptive mutation.
 A particular mutation type is defined by its distribution of fitness effects (DFE) and its dominance coefficient.  Once a mutation type
 is defined, a draw from its DFE can be generated to determine the selection coefficient of a particular mutation of that type.
 
 */

#ifndef __SLiM__mutation_type__
#define __SLiM__mutation_type__


#include <vector>
#include <string>


class MutationType
{
	//	This class has its copy constructor and assignment operator disabled, to prevent accidental copying.

public:
	
	// a mutation type is specified by the DFE and the dominance coefficient
	//
	// DFE options: f: fixed (s) 
	//              e: exponential (mean s)
	//              g: gamma distribution (mean s,shape)
	//
	// examples: synonymous, nonsynonymous, adaptive, etc.
	
	int mutation_type_id_;						// the id by which this mutation type is indexed in the chromosome
	float dominance_coeff_;						// dominance coefficient (h)
	char dfe_type_;								// distribution of fitness effects (DFE) type (f: fixed, g: gamma, e: exponential)
	std::vector<double> dfe_parameters_;		// DFE parameters
	
	MutationType(const MutationType&) = delete;					// no copying
	MutationType& operator=(const MutationType&) = delete;		// no copying
	MutationType(int p_mutation_type_id, double p_dominance_coeff, char p_dfe_type, std::vector<double> p_dfe_parameters);
	
	double DrawSelectionCoefficient() const;					// draw a selection coefficient from this mutation type's DFE
};

// support stream output of MutationType, for debugging
std::ostream &operator<<(std::ostream &p_outstream, const MutationType &p_mutation_type);


#endif /* defined(__SLiM__mutation_type__) */




































































