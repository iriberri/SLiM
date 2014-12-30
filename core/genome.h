//
//  genome.h
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
 
 The class Genome represents a particular genome, defined as a vector of mutations.  Each individual in the simulation has a genome,
 which determines that individual's fitness (from the fitness effects of all of the mutations possessed).
 
 */

#ifndef __SLiM__genome__
#define __SLiM__genome__


#include <vector>

#include "mutation.h"
#include "stacktrace.h"


class Genome
{
	// This class has a restricted copying policy; see below

private:
	
	int mutation_count_ = 0;				// the number of entries presently in mutations_
	int mutation_capacity_ = 0;				// the capacity of mutations_
	const Mutation **mutations_ = nullptr;	// OWNED POINTER: a pointer to a malloced array of pointers to const Mutation objects
	
#ifdef DEBUG
	static bool s_log_copy_and_assign_;						// true if logging is disabled (see below)
#endif
	
public:
	
	//
	//	This class should not be copied, in general, but the default copy constructor and assignment operator cannot be entirely
	//	disabled, because we want to keep instances of this class inside STL containers.  We therefore override the default copy
	//	constructor and the default assignment operator to log whenever they are called.  This is intended to reduce the risk of
	//	unintentional copying.  Logging can be disabled by bracketing with LogGenomeCopyAndAssign() when appropriate, or by
	//	using copy_from_genome(), which is the preferred way to intentionally copy a Genome.
	//
	Genome(const Genome &p_original);
	Genome& operator= (const Genome &p_original);
#ifdef DEBUG
	static bool LogGenomeCopyAndAssign(bool p_log);			// returns the old value; save and restore that value!
#endif
	
	Genome() = default;										// default constructor
	~Genome()
	{
		free(mutations_);
	}
	
	inline const Mutation *const & operator[] (int index) const		// [] returns a reference to a pointer to Mutation; this is the const-pointer variant
	{
		return mutations_[index];
	}
	
	inline const Mutation *& operator[] (int index)					// [] returns a reference to a pointer to Mutation; this is the non-const-pointer variant
	{
		return mutations_[index];
	}
	
	inline int size(void)
	{
		return mutation_count_;
	}
	
	inline void clear(void)
	{
		mutation_count_ = 0;
	}
	
	inline void pop_back()
	{
		if (mutation_count_ > 0)	// the standard says that popping an empty vector results in undefined behavior; this seems reasonable
			--mutation_count_;
	}
	
	inline void push_back(const Mutation *p_mutation)
	{
		if (mutation_count_ == mutation_capacity_)
		{
			if (!mutation_capacity_)
			{
				mutation_capacity_ = 16;		// start with room for 16 pointers; the hope is that for many simulations this will avoid realloc entirely
				mutations_ = (const Mutation **)malloc(16 * sizeof(Mutation*));
			}
			else
			{
				mutation_capacity_ <<= 1;		// double the number of pointers we can hold
				mutations_ = (const Mutation **)realloc(mutations_, mutation_capacity_ * sizeof(Mutation*));
			}
		}
		
		// Now we are guaranteed to have enough memory, so copy the pointer in
		// (unless malloc/realloc failed, which we're not going to worry about!)
		*(mutations_ + mutation_count_) = p_mutation;
		++mutation_count_;
	}
	
	inline void insert_sorted_mutation(const Mutation *p_mutation)
	{
		// first push it back on the end, which deals with capacity issues
		push_back(p_mutation);
		
		// if it was our first element, then we're done; this would work anyway, but since it is extremely common let's short-circuit it
		if (mutation_count_ == 1)
			return;
		
		// then find the proper position for it
		const Mutation **sort_position = begin_pointer();
		const Mutation **end_position = end_pointer() - 1;		// the position of the newly added element
		
		for (sort_position = mutations_; sort_position != end_position; ++sort_position)
			if (CompareMutations(p_mutation, *sort_position))	// if (p_mutation->position_ < (*sort_position)->position_)
				break;
		
		// if we got all the way to the end, then the mutation belongs at the end, so we're done
		if (sort_position == end_position)
			return;
		
		// the new mutation has a position less than that at sort_position, so we need to move everybody upward
		memmove(sort_position + 1, sort_position, (char *)end_position - (char *)sort_position);
		
		// finally, put the mutation where it belongs
		*sort_position = p_mutation;
	}
	
	inline void insert_sorted_mutation_if_unique(const Mutation *p_mutation)
	{
		// first push it back on the end, which deals with capacity issues
		push_back(p_mutation);
		
		// if it was our first element, then we're done; this would work anyway, but since it is extremely common let's short-circuit it
		if (mutation_count_ == 1)
			return;
		
		// then find the proper position for it
		const Mutation **sort_position = begin_pointer();
		const Mutation **end_position = end_pointer() - 1;		// the position of the newly added element
		
		for (sort_position = mutations_; sort_position != end_position; ++sort_position)
		{
			if (CompareMutations(p_mutation, *sort_position))	// if (p_mutation->position_ < (*sort_position)->position_)
			{
				break;
			}
			else if (EqualMutations(p_mutation, *sort_position))
			{
				// We are only supposed to insert the mutation if it is unique, and apparently it is not; discard it off the end
				--mutation_count_;
				return;
			}
		}
		
		// if we got all the way to the end, then the mutation belongs at the end, so we're done
		if (sort_position == end_position)
			return;
		
		// the new mutation has a position less than that at sort_position, so we need to move everybody upward
		memmove(sort_position + 1, sort_position, (char *)end_position - (char *)sort_position);
		
		// finally, put the mutation where it belongs
		*sort_position = p_mutation;
	}
	
	inline void copy_from_genome(const Genome &p_source_genome)
	{
		int source_mutation_count = p_source_genome.mutation_count_;
		
		// first we need to ensure that we have sufficient capacity
		if (source_mutation_count > mutation_capacity_)
		{
			mutation_capacity_ = p_source_genome.mutation_capacity_;		// just use the same capacity as the source
			mutations_ = (const Mutation **)realloc(mutations_, mutation_capacity_ * sizeof(Mutation*));
		}
		
		// then copy all pointers from the source to ourselves
		memcpy(mutations_, p_source_genome.mutations_, source_mutation_count * sizeof(Mutation*));
		mutation_count_ = source_mutation_count;
	}
	
	inline const Mutation **begin_pointer(void) const
	{
		return mutations_;
	}
	
	inline const Mutation **end_pointer(void) const
	{
		return mutations_ + mutation_count_;
	}
	
	inline const Mutation *& back(void) const				// returns a reference to a pointer to a const Mutation
	{
		return *(mutations_ + (mutation_count_ - 1));
	}
};

// set p_merge_genome to be a merged genome consisting only of the mutations that are present in both p_genome1 and p_genome2
// NOTE this function is not used in the present design; it has been obsoleted by Population::ManageMutationReferencesAndRemoveFixedMutations(int p_generation).
void GenomeWithFixedMutations(const Genome &p_genome1, const Genome &p_genome2, Genome *p_merge_genome);

// set p_merge_genome to be a merged genome consisting only of the mutations in p_genome1 that are not in p_genome2
void GenomeWithPolymorphicMutations(const Genome &p_genome1, const Genome &p_genome2, Genome *p_merge_genome);


#endif /* defined(__SLiM__genome__) */



































































