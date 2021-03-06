//
//  slim_sim.cpp
//  SLiM
//
//  Created by Ben Haller on 12/26/14.
//  Copyright (c) 2014-2016 Philipp Messer.  All rights reserved.
//	A product of the Messer Lab, http://messerlab.org/slim/
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


#include "slim_sim.h"
#include "eidos_test.h"
#include "eidos_interpreter.h"
#include "eidos_call_signature.h"
#include "eidos_property_signature.h"
#include "eidos_ast_node.h"
#include "individual.h"
#include "polymorphism.h"

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <typeinfo>
#include <memory>
#include <string>
#include <utility>


using std::string;
using std::endl;
using std::istream;
using std::istringstream;
using std::ifstream;
using std::vector;


SLiMSim::SLiMSim(std::istream &p_infile) : population_(*this), self_symbol_(gID_sim, EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Object_singleton(this, gSLiM_SLiMSim_Class))), x_experiments_enabled_(false)
{
	// set up the symbol table we will use for all of our constants
	simulation_constants_ = new EidosSymbolTable(EidosSymbolTableType::kContextConstantsTable, gEidosConstantsSymbolTable);
	
	// set up the function map with the base Eidos functions plus zero-gen functions, since we're in an initial state
	simulation_functions_ = *EidosInterpreter::BuiltInFunctionMap();
	AddZeroGenerationFunctionsToMap(simulation_functions_);
	
	// read all configuration information from the input file
	p_infile.clear();
	p_infile.seekg(0, std::fstream::beg);
	InitializeFromFile(p_infile);
}

SLiMSim::SLiMSim(const char *p_input_file) : population_(*this), self_symbol_(gID_sim, EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Object_singleton(this, gSLiM_SLiMSim_Class))), x_experiments_enabled_(false)
{
	// set up the symbol table we will use for all of our constants
	simulation_constants_ = new EidosSymbolTable(EidosSymbolTableType::kContextConstantsTable, gEidosConstantsSymbolTable);
	
	// set up the function map with the base Eidos functions plus zero-gen functions, since we're in an initial state
	simulation_functions_ = *EidosInterpreter::BuiltInFunctionMap();
	AddZeroGenerationFunctionsToMap(simulation_functions_);
	
	// open our file stream
	std::ifstream infile(p_input_file);
	
	if (!infile.is_open())
		EIDOS_TERMINATION << std::endl << "ERROR (SLiMSim::SLiMSim): could not open input file: " << p_input_file << "." << eidos_terminate();
	
	// read all configuration information from the input file
	infile.seekg(0, std::fstream::beg);
	InitializeFromFile(infile);
}

SLiMSim::~SLiMSim(void)
{
	//EIDOS_ERRSTREAM << "SLiMSim::~SLiMSim" << std::endl;
	
	delete simulation_constants_;
	simulation_constants_ = nullptr;
	
	simulation_functions_.clear();
	
	for (auto mutation_type : mutation_types_)
		delete mutation_type.second;
	
	for (auto genomic_element_type : genomic_element_types_)
		delete genomic_element_type.second;
	
	for (auto interaction_type : interaction_types_)
		delete interaction_type.second;
	
	for (auto script_block : script_blocks_)
		delete script_block;
	
	// All the script blocks that refer to the script are now gone
	delete script_;
	
	// Dispose of mutation run experiment data
	if (x_experiments_enabled_)
	{
		if (x_current_runtimes_)
			free(x_current_runtimes_);
		
		if (x_previous_runtimes_)
			free(x_previous_runtimes_);
	}
}

void SLiMSim::InitializeRNGFromSeed(unsigned long int *p_override_seed_ptr)
{
	// track the random number seed given, if there is one
	unsigned long int rng_seed = (p_override_seed_ptr ? *p_override_seed_ptr : EidosGenerateSeedFromPIDAndTime());
	
	EidosInitializeRNGFromSeed(rng_seed);
	
	if (DEBUG_INPUT)
		SLIM_OUTSTREAM << "// Initial random seed:\n" << rng_seed << "\n" << endl;
}

void SLiMSim::InitializeFromFile(std::istream &p_infile)
{
	// Reset error position indicators used by SLiMgui
	EidosScript::ClearErrorPosition();
	
	// Read in the file; going through stringstream is fast...
	std::stringstream buffer;
	
	buffer << p_infile.rdbuf();
	
	// Tokenize and parse
	script_ = new SLiMEidosScript(buffer.str());
	
	// Set up top-level error-reporting info
	gEidosCurrentScript = script_;
	gEidosExecutingRuntimeScript = false;
	
	script_->Tokenize();
	script_->ParseSLiMFileToAST();
	
	// Extract SLiMEidosBlocks from the parse tree
	const EidosASTNode *root_node = script_->AST();
	
	for (EidosASTNode *script_block_node : root_node->children_)
	{
		SLiMEidosBlock *new_script_block = new SLiMEidosBlock(script_block_node);
		
		AddScriptBlock(new_script_block, nullptr, new_script_block->root_node_->children_[0]->token_);
	}
	
	// Reset error position indicators used by SLiMgui
	EidosScript::ClearErrorPosition();
	
	// Zero out error-reporting info so raises elsewhere don't get attributed to this script
	gEidosCurrentScript = nullptr;
	gEidosExecutingRuntimeScript = false;
}

// get one line of input, sanitizing by removing comments and whitespace; used only by SLiMSim::InitializePopulationFromTextFile
void GetInputLine(istream &p_input_file, string &p_line);
void GetInputLine(istream &p_input_file, string &p_line)
{
	getline(p_input_file, p_line);
	
	// remove all after "//", the comment start sequence
	// BCH 16 Dec 2014: note this was "/" in SLiM 1.8 and earlier, changed to allow full filesystem paths to be specified.
	if (p_line.find("//") != string::npos)
		p_line.erase(p_line.find("//"));
	
	// remove leading and trailing whitespace (spaces and tabs)
	p_line.erase(0, p_line.find_first_not_of(" \t"));
	p_line.erase(p_line.find_last_not_of(" \t") + 1);
}

int SLiMSim::FormatOfPopulationFile(const char *p_file)
{
	if (p_file)
	{
		ifstream infile(p_file, std::ios::in | std::ios::binary);
		
		if (!infile.is_open() || infile.eof())
			return -1;
		
		// Determine the file length
		infile.seekg(0, std::ios_base::end);
		std::size_t file_size = infile.tellg();
		
		// Determine the file format
		if (file_size >= 4)
		{
			char file_chars[4];
			int32_t file_endianness_tag;
			
			infile.seekg(0, std::ios_base::beg);
			infile.read(&file_chars[0], 4);
			
			infile.seekg(0, std::ios_base::beg);
			infile.read(reinterpret_cast<char *>(&file_endianness_tag), sizeof file_endianness_tag);
			
			if ((file_chars[0] == '#') && (file_chars[1] == 'O') && (file_chars[2] == 'U') && (file_chars[3] == 'T'))
				return 1;
			else if (file_endianness_tag == 0x12345678)
				return 2;
		}
	}
	
	return 0;
}

slim_generation_t SLiMSim::InitializePopulationFromFile(const char *p_file, EidosInterpreter *p_interpreter)
{
	int file_format = FormatOfPopulationFile(p_file);	// -1 is file does not exist, 0 is format unrecognized, 1 is text, 2 is binary
	
	if (file_format == -1)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromFile): initialization file does not exist or is empty." << eidos_terminate();
	if (file_format == 0)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromFile): initialization file is invalid." << eidos_terminate();
	
	{
		if (p_interpreter)
		{
			EidosSymbolTable &symbols = p_interpreter->SymbolTable();
			std::vector<std::string> all_symbols = symbols.AllSymbols();
			std::vector<EidosGlobalStringID> symbols_to_remove;
			
			for (string symbol_name : all_symbols)
			{
				EidosGlobalStringID symbol_ID = EidosGlobalStringIDForString(symbol_name);
				EidosValue_SP symbol_value = symbols.GetValueOrRaiseForSymbol(symbol_ID);
				
				if (symbol_value->Type() == EidosValueType::kValueObject)
				{
					const EidosObjectClass *symbol_class = static_pointer_cast<EidosValue_Object>(symbol_value)->Class();
					
					if ((symbol_class == gSLiM_Subpopulation_Class) || (symbol_class == gSLiM_Genome_Class) || (symbol_class == gSLiM_Individual_Class) || (symbol_class == gSLiM_Mutation_Class) || (symbol_class == gSLiM_Substitution_Class))
						symbols_to_remove.emplace_back(symbol_ID);
				}
			}
			
			for (EidosGlobalStringID symbol_ID : symbols_to_remove)
				symbols.RemoveConstantForSymbol(symbol_ID);
		}
		
		// invalidate interactions, since any cached interaction data depends on the subpopulations and individuals
		for (auto int_type = interaction_types_.begin(); int_type != interaction_types_.end(); ++int_type)
			int_type->second->Invalidate();
		
		// then we dispose of all existing subpopulations, mutations, etc.
		population_.RemoveAllSubpopulationInfo();
	}
	
	if (file_format == 1)
		return _InitializePopulationFromTextFile(p_file, p_interpreter);
	else if (file_format == 2)
		return _InitializePopulationFromBinaryFile(p_file, p_interpreter);
	else
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromFile): unreconized format code." << eidos_terminate();
}

slim_generation_t SLiMSim::_InitializePopulationFromTextFile(const char *p_file, EidosInterpreter *p_interpreter)
{
	slim_generation_t file_generation;
	std::map<slim_polymorphismid_t,MutationIndex> mutations;
	string line, sub; 
	ifstream infile(p_file);
	
	if (!infile.is_open())
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): could not open initialization file." << eidos_terminate();
	
	// Parse the first line, to get the generation
	{
		GetInputLine(infile, line);
	
		istringstream iss(line);
		
		iss >> sub;		// #OUT:
		
		iss >> sub;		// generation
		int64_t generation_long = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
		file_generation = SLiMCastToGenerationTypeOrRaise(generation_long);
	}
	
	// Read and ignore initial stuff until we hit the Populations section
	int64_t file_version = 0;	// initially unknown; we will leave this as 0 for versions < 3, for now
	
	while (!infile.eof())
	{
		GetInputLine(infile, line);
		
		// Starting in SLiM 3, we will handle a Version line if we see one in passing
		if (line.find("Version:") != string::npos)
		{
			istringstream iss(line);
			
			iss >> sub;		// Version:
			iss >> sub;		// version number
			
			file_version = (int64_t)EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
			
			if ((file_version != 1) && (file_version != 2) && (file_version != 3))
				EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): unrecognized version." << eidos_terminate();
			
			continue;
		}
		
		if (line.find("Populations") != string::npos)
			break;
	}
	
	// Now we are in the Populations section; read and instantiate each population until we hit the Mutations section
	while (!infile.eof())
	{ 
		GetInputLine(infile, line);
		
		if (line.length() == 0)
			continue;
		if (line.find("Mutations") != string::npos)
			break;
		
		istringstream iss(line);
		
		iss >> sub;
		slim_objectid_t subpop_index = SLiMEidosScript::ExtractIDFromStringWithPrefix(sub.c_str(), 'p', nullptr);
		
		iss >> sub;
		int64_t subpop_size_long = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
		slim_popsize_t subpop_size = SLiMCastToPopsizeTypeOrRaise(subpop_size_long);
		
		// SLiM 2.0 output format has <H | S <ratio>> here; if that is missing or "H" is given, the population is hermaphroditic and the ratio given is irrelevant
		double sex_ratio = 0.0;
		
		if (iss >> sub)
		{
			if (sub == "S")
			{
				iss >> sub;
				sex_ratio = EidosInterpreter::FloatForString(sub, nullptr);
			}
		}
		
		// Create the population population
		Subpopulation *new_subpop = population_.AddSubpopulation(subpop_index, subpop_size, sex_ratio);
		
		// define a new Eidos variable to refer to the new subpopulation
		EidosSymbolTableEntry &symbol_entry = new_subpop->SymbolTableEntry();
		
		if (p_interpreter && p_interpreter->SymbolTable().ContainsSymbol(symbol_entry.first))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): new subpopulation symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate();
		
		simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
	}
	
	// Now we are in the Mutations section; read and instantiate all mutations and add them to our map and to the registry
	while (!infile.eof()) 
	{
		GetInputLine(infile, line);
		
		if (line.length() == 0)
			continue;
		if (line.find("Genomes") != string::npos)
			break;
		if (line.find("Individuals") != string::npos)	// SLiM 2.0 added this section
			break;
		
		istringstream iss(line);
		
		iss >> sub;
		int64_t polymorphismid_long = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
		slim_polymorphismid_t polymorphism_id = SLiMCastToPolymorphismidTypeOrRaise(polymorphismid_long);
		
		// Added in version 2 output, starting in SLiM 2.1
		iss >> sub;
		slim_mutationid_t mutation_id;
		
		if (sub[0] == 'm')	// autodetect whether we are parsing version 1 or version 2 output
		{
			mutation_id = polymorphism_id;		// when parsing version 1 output, we use the polymorphism id as the mutation id
		}
		else
		{
			mutation_id = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
			
			iss >> sub;		// queue up sub for mutation_type_id
		}
		
		slim_objectid_t mutation_type_id = SLiMEidosScript::ExtractIDFromStringWithPrefix(sub.c_str(), 'm', nullptr);
		
		iss >> sub;
		int64_t position_long = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
		slim_position_t position = SLiMCastToPositionTypeOrRaise(position_long);
		
		iss >> sub;
		double selection_coeff = EidosInterpreter::FloatForString(sub, nullptr);
		
		iss >> sub;		// dominance coefficient, which is given in the mutation type; we check below that the value read matches the mutation type
		double dominance_coeff = EidosInterpreter::FloatForString(sub, nullptr);
		
		iss >> sub;
		slim_objectid_t subpop_index = SLiMEidosScript::ExtractIDFromStringWithPrefix(sub.c_str(), 'p', nullptr);
		
		iss >> sub;
		int64_t generation_long = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
		slim_generation_t generation = SLiMCastToGenerationTypeOrRaise(generation_long);
		
		// look up the mutation type from its index
		auto found_muttype_pair = mutation_types_.find(mutation_type_id);
		
		if (found_muttype_pair == mutation_types_.end()) 
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): mutation type m"<< mutation_type_id << " has not been defined." << eidos_terminate();
		
		MutationType *mutation_type_ptr = found_muttype_pair->second;
		
		if (fabs(mutation_type_ptr->dominance_coeff_ - dominance_coeff) > 0.001)	// a reasonable tolerance to allow for I/O roundoff
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): mutation type m"<< mutation_type_id << " has dominance coefficient " << mutation_type_ptr->dominance_coeff_ << " that does not match the population file dominance coefficient of " << dominance_coeff << "." << eidos_terminate();
		
		// construct the new mutation; NOTE THAT THE STACKING POLICY IS NOT CHECKED HERE, AS THIS IS NOT CONSIDERED THE ADDITION OF A MUTATION!
		MutationIndex new_mut_index = SLiM_NewMutationFromBlock();
		
		new (gSLiM_Mutation_Block + new_mut_index) Mutation(mutation_id, mutation_type_ptr, position, selection_coeff, subpop_index, generation);
		
		// add it to our local map, so we can find it when making genomes, and to the population's mutation registry
		mutations.insert(std::pair<slim_polymorphismid_t,MutationIndex>(polymorphism_id, new_mut_index));
		population_.mutation_registry_.emplace_back(new_mut_index);
		
		// all mutations seen here will be added to the simulation somewhere, so check and set pure_neutral_ and all_pure_neutral_DFE_
		if (selection_coeff != 0.0)
		{
			pure_neutral_ = false;
			mutation_type_ptr->all_pure_neutral_DFE_ = false;
		}
	}
	
	population_.cached_tally_genome_count_ = 0;
	
	// If there is an Individuals section (added in SLiM 2.0), we now need to parse it since it might contain spatial positions
	if (line.find("Individuals") != string::npos)
	{
		while (!infile.eof()) 
		{
			GetInputLine(infile, line);
			
			if (line.length() == 0)
				continue;
			if (line.find("Genomes") != string::npos)
				break;
			
			istringstream iss(line);
			
			iss >> sub;		// pX:iY – individual identifier
			int pos = static_cast<int>(sub.find_first_of(":"));
			const char *subpop_id_string = sub.substr(0, pos).c_str();
			slim_objectid_t subpop_id = SLiMEidosScript::ExtractIDFromStringWithPrefix(subpop_id_string, 'p', nullptr);
			const char *individual_index_string = sub.substr(pos + 1, string::npos).c_str();
			
			if (individual_index_string[0] != 'i')
				EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): reference to individual is malformed." << eidos_terminate();
			
			int64_t individual_index = EidosInterpreter::NonnegativeIntegerForString(individual_index_string + 1, nullptr);
			
			auto subpop_pair = population_.find(subpop_id);
			
			if (subpop_pair == population_.end())
				EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): referenced subpopulation p" << subpop_id << " not defined." << eidos_terminate();
			
			Subpopulation &subpop = *subpop_pair->second;
			
			if (individual_index >= subpop.parent_subpop_size_)
				EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): referenced individual i" << individual_index << " is out of range." << eidos_terminate();
			
			Individual &individual = subpop.parent_individuals_[individual_index];
			
			iss >> sub;			// individual sex identifier (F/M/H) – added in SLiM 2.1, so we need to be robust if it is missing
			
			if ((sub == "F") || (sub == "M") || (sub == "H"))
				iss >> sub;
			
			;					// pX:Y – genome 1 identifier, which we do not presently need to parse [already fetched]
			iss >> sub;			// pX:Y – genome 2 identifier, which we do not presently need to parse
			
			if (spatial_dimensionality_ >= 1)
			{
				if (iss >> sub)		// spatial position x
					individual.spatial_x_ = EidosInterpreter::FloatForString(sub, nullptr);
			}
			
			if (spatial_dimensionality_ >= 2)
			{
				if (iss >> sub)		// spatial position y
					individual.spatial_y_ = EidosInterpreter::FloatForString(sub, nullptr);
			}
			
			if (spatial_dimensionality_ >= 3)
			{
				if (iss >> sub)		// spatial position z
					individual.spatial_z_ = EidosInterpreter::FloatForString(sub, nullptr);
			}
			
			if (iss >> sub)
				EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): output spatial dimensionality does not match that of the simulation." << eidos_terminate();
		}
	}
	
	// Now we are in the Genomes section, which should take us to the end of the file
	Mutation *mut_block_ptr = gSLiM_Mutation_Block;
	
	while (!infile.eof())
	{
		GetInputLine(infile, line);
		
		if (line.length() == 0)
			continue;
		
		istringstream iss(line);
		
		iss >> sub;
		int pos = static_cast<int>(sub.find_first_of(":"));
		const char *subpop_id_string = sub.substr(0, pos).c_str();
		slim_objectid_t subpop_id = SLiMEidosScript::ExtractIDFromStringWithPrefix(subpop_id_string, 'p', nullptr);
		
		auto subpop_pair = population_.find(subpop_id);
		
		if (subpop_pair == population_.end())
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): referenced subpopulation p" << subpop_id << " not defined." << eidos_terminate();
		
		Subpopulation &subpop = *subpop_pair->second;
		
		sub.erase(0, pos + 1);	// remove the subpop_id and the colon
		int64_t genome_index_long = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
		
		if ((genome_index_long < 0) || (genome_index_long > SLIM_MAX_SUBPOP_SIZE * 2))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): genome index out of permitted range." << eidos_terminate();
		slim_popsize_t genome_index = static_cast<slim_popsize_t>(genome_index_long);	// range-check is above since we need to check against SLIM_MAX_SUBPOP_SIZE * 2
		
		Genome &genome = subpop.parent_genomes_[genome_index];
		
		// Now we might have [A|X|Y] (SLiM 2.0), or we might have the first mutation id - or we might have nothing at all
		if (iss >> sub)
		{
			// check whether this token is a genome type
			if ((sub.compare(gStr_A) == 0) || (sub.compare(gStr_X) == 0) || (sub.compare(gStr_Y) == 0))
			{
				// Let's do a little error-checking against what has already been instantiated for us...
				if ((sub.compare(gStr_A) == 0) && genome.Type() != GenomeType::kAutosome)
					EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): genome is specified as A (autosome), but the instantiated genome does not match." << eidos_terminate();
				if ((sub.compare(gStr_X) == 0) && genome.Type() != GenomeType::kXChromosome)
					EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): genome is specified as X (X-chromosome), but the instantiated genome does not match." << eidos_terminate();
				if ((sub.compare(gStr_Y) == 0) && genome.Type() != GenomeType::kYChromosome)
					EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): genome is specified as Y (Y-chromosome), but the instantiated genome does not match." << eidos_terminate();
				
				if (iss >> sub)
				{
					if (sub == "<null>")
					{
						if (!genome.IsNull())
							EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): genome is specified as null, but the instantiated genome is non-null." << eidos_terminate();
						
						continue;	// this line is over
					}
					else
					{
						if (genome.IsNull())
							EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): genome is specified as non-null, but the instantiated genome is null." << eidos_terminate();
						
						// drop through, and sub will be interpreted as a mutation id below
					}
				}
				else
					continue;
			}
			
			int32_t mutrun_length_ = genome.mutrun_length_;
			int current_mutrun_index = -1;
			MutationRun *current_mutrun = nullptr;
			
			do
			{
				int64_t polymorphismid_long = EidosInterpreter::NonnegativeIntegerForString(sub, nullptr);
				slim_polymorphismid_t polymorphism_id = SLiMCastToPolymorphismidTypeOrRaise(polymorphismid_long);
				
				auto found_mut_pair = mutations.find(polymorphism_id);
				
				if (found_mut_pair == mutations.end()) 
					EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromTextFile): polymorphism " << polymorphism_id << " has not been defined." << eidos_terminate();
				
				MutationIndex mutation = found_mut_pair->second;
				int mutrun_index = (mut_block_ptr + mutation)->position_ / mutrun_length_;
				
				if (mutrun_index != current_mutrun_index)
				{
					current_mutrun_index = mutrun_index;
					genome.WillModifyRun(current_mutrun_index);
					
					current_mutrun = genome.mutruns_[mutrun_index].get();
				}
				
				current_mutrun->emplace_back(mutation);
			}
			while (iss >> sub);
		}
	}
	
	// It's a little unclear how we ought to clean up after ourselves, and this is a continuing source of bugs.  We could be loading
	// a new population in an early() event, in a late() event, or in between generations in SLiMgui using its Import Population command.
	// The safest avenue seems to be to just do all the bookkeeping we can think of: tally frequencies, calculate fitnesses, and
	// survey the population for SLiMgui.  This will lead to some of these actions being done at an unusual time in the generation cycle,
	// though, and will cause some things to be done unnecessarily (because they are not normally up-to-date at the current generation
	// cycle stage anyway) or done twice (which could be particularly problematic for fitness() callbacks).  Nevertheless, this seems
	// like the best policy, at least until shown otherwise...  BCH 11 June 2016
	
	// BCH 5 April 2017: Well, it has been shown otherwise.  Now that interactions have been added, fitness() callbacks often depend on
	// them, which means the interactions need to be evaluated, which means we can't evaluate fitness values yet; we need to give the
	// user's script a chance to evaluate the interactions.  This was always a problem, really; fitness() callbacks might have needed
	// some external state to be set up that would on the population state.  But now it is a glaring problem, and forces us to revise
	// our policy.  For backward compatibility, we will keep the old behavior if reading a file that is version 2 or earlier; a bit
	// weird, but probably nobody will ever even notice...
	
	// As of SLiM 2.1, we change the generation as a side effect of loading; otherwise we can't correctly update our state here!
	generation_ = file_generation;
	
	// Re-tally mutation references so we have accurate frequency counts for our new mutations
	population_.UniqueMutationRuns();
	population_.MaintainRegistry();
	
	if (file_version <= 2)
	{
		// Now that we have the info on everybody, update fitnesses so that we're ready to run the next generation
		// used to be generation + 1; removing that 18 Feb 2016 BCH
		
		nonneutral_change_counter_++;			// trigger unconditional nonneutral mutation caching inside UpdateFitness()
		last_nonneutral_regime_ = 3;			// this means "unpredictable callbacks", will trigger a recache next generation
		
		for (auto muttype_iter : mutation_types_)
			(muttype_iter.second)->subject_to_fitness_callback_ = true;			// we're not doing RecalculateFitness()'s work, so play it safe
		
		for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
		{
			slim_objectid_t subpop_id = subpop_pair.first;
			Subpopulation *subpop = subpop_pair.second;
			std::vector<SLiMEidosBlock*> fitness_callbacks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosFitnessCallback, -1, -1, subpop_id);
			std::vector<SLiMEidosBlock*> global_fitness_callbacks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback, -2, -1, subpop_id);
			
			subpop->UpdateFitness(fitness_callbacks, global_fitness_callbacks);
		}
		
#ifdef SLIMGUI
		// Let SLiMgui survey the population for mean fitness and such, if it is our target
		population_.SurveyPopulation();
#endif
	}
	
	return file_generation;
}

slim_generation_t SLiMSim::_InitializePopulationFromBinaryFile(const char *p_file, EidosInterpreter *p_interpreter)
{
	std::size_t file_size = 0;
	slim_generation_t file_generation;
	int32_t spatial_output_count;
	
	// Read file into buf
	ifstream infile(p_file, std::ios::in | std::ios::binary);
	
	if (!infile.is_open() || infile.eof())
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): could not open initialization file." << eidos_terminate();
	
	// Determine the file length
	infile.seekg(0, std::ios_base::end);
	file_size = infile.tellg();
	
	// Read in the entire file; we assume we have enough memory, for now
	std::unique_ptr<char> raii_buf(new char[file_size]);
	char *buf = raii_buf.get();
	
	if (!buf)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): could not allocate input buffer." << eidos_terminate();
	
	char *buf_end = buf + file_size;
	char *p = buf;
	
	infile.seekg(0, std::ios_base::beg);
	infile.read(buf, file_size);
	
	// Close the file; we will work only with our buffer from here on
	infile.close();
	
	int32_t section_end_tag;
	int32_t file_version;
	
	// Header beginning, to check endianness and determine file version
	{
		int32_t endianness_tag, version_tag;
		
		if (p + sizeof(endianness_tag) + sizeof(version_tag) > buf_end)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF while reading header." << eidos_terminate();
		
		endianness_tag = *(int32_t *)p;
		p += sizeof(endianness_tag);
		
		version_tag = *(int32_t *)p;
		p += sizeof(version_tag);
		
		if (endianness_tag != 0x12345678)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): endianness mismatch." << eidos_terminate();
		if ((version_tag != 1) && (version_tag != 2) && (version_tag != 3))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unrecognized version." << eidos_terminate();
		
		file_version = version_tag;
	}
	
	// Header section
	{
		int32_t double_size;
		double double_test;
		int32_t slim_generation_t_size, slim_position_t_size, slim_objectid_t_size, slim_popsize_t_size, slim_refcount_t_size, slim_selcoeff_t_size, slim_mutationid_t_size, slim_polymorphismid_t_size;
		int header_length = sizeof(double_size) + sizeof(double_test) + sizeof(slim_generation_t_size) + sizeof(slim_position_t_size) + sizeof(slim_objectid_t_size) + sizeof(slim_popsize_t_size) + sizeof(slim_refcount_t_size) + sizeof(slim_selcoeff_t_size) + sizeof(file_generation) + sizeof(section_end_tag);
		
		if (file_version >= 2)
			header_length += sizeof(slim_mutationid_t_size) + sizeof(slim_polymorphismid_t_size);
		
		if (p + header_length > buf_end)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF while reading header." << eidos_terminate();
		
		double_size = *(int32_t *)p;
		p += sizeof(double_size);
		
		double_test = *(double *)p;
		p += sizeof(double_test);
		
		slim_generation_t_size = *(int32_t *)p;
		p += sizeof(slim_generation_t_size);
		
		slim_position_t_size = *(int32_t *)p;
		p += sizeof(slim_position_t_size);
		
		slim_objectid_t_size = *(int32_t *)p;
		p += sizeof(slim_objectid_t_size);
		
		slim_popsize_t_size = *(int32_t *)p;
		p += sizeof(slim_popsize_t_size);
		
		slim_refcount_t_size = *(int32_t *)p;
		p += sizeof(slim_refcount_t_size);
		
		slim_selcoeff_t_size = *(int32_t *)p;
		p += sizeof(slim_selcoeff_t_size);
		
		if (file_version >= 2)
		{
			slim_mutationid_t_size = *(int32_t *)p;
			p += sizeof(slim_mutationid_t_size);
			
			slim_polymorphismid_t_size = *(int32_t *)p;
			p += sizeof(slim_polymorphismid_t_size);
		}
		else
		{
			// Version 1 file; backfill correct values
			slim_mutationid_t_size = sizeof(slim_mutationid_t);
			slim_polymorphismid_t_size = sizeof(slim_polymorphismid_t);
		}
		
		file_generation = *(slim_generation_t *)p;
		p += sizeof(file_generation);
		
		if (file_version >= 3)
		{
			spatial_output_count = *(int32_t *)p;
			p += sizeof(spatial_output_count);
		}
		else
		{
			// Version 1 or 2 file; backfill correct value
			spatial_output_count = 0;
		}
		
		section_end_tag = *(int32_t *)p;
		p += sizeof(section_end_tag);
		
		if (double_size != sizeof(double))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): sizeof(double) mismatch." << eidos_terminate();
		if (double_test != 1234567890.0987654321)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): double format mismatch." << eidos_terminate();
		if ((slim_generation_t_size != sizeof(slim_generation_t)) ||
			(slim_position_t_size != sizeof(slim_position_t)) ||
			(slim_objectid_t_size != sizeof(slim_objectid_t)) ||
			(slim_popsize_t_size != sizeof(slim_popsize_t)) ||
			(slim_refcount_t_size != sizeof(slim_refcount_t)) ||
			(slim_selcoeff_t_size != sizeof(slim_selcoeff_t)) ||
			(slim_mutationid_t_size != sizeof(slim_mutationid_t)) ||
			(slim_polymorphismid_t_size != sizeof(slim_polymorphismid_t)))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): SLiM datatype size mismatch." << eidos_terminate();
		if ((spatial_output_count < 0) || (spatial_output_count > 3))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): spatial output count out of range." << eidos_terminate();
		if ((spatial_output_count > 0) && (spatial_output_count != spatial_dimensionality_))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): output spatial dimensionality does not match that of the simulation." << eidos_terminate();
		if (section_end_tag != (int32_t)0xFFFF0000)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): missing section end after header." << eidos_terminate();
	}
	
	// Populations section
	while (true)
	{
		int32_t subpop_start_tag;
		slim_objectid_t subpop_id;
		slim_popsize_t subpop_size;
		int32_t sex_flag;
		double subpop_sex_ratio;
		
		// If there isn't enough buffer left to read a full subpop record, we assume we are done with this section
		if (p + sizeof(subpop_start_tag) + sizeof(subpop_id) + sizeof(subpop_size) + sizeof(sex_flag) + sizeof(subpop_sex_ratio) > buf_end)
			break;
		
		// If the first int32_t is not a subpop start tag, then we are done with this section
		subpop_start_tag = *(int32_t *)p;
		if (subpop_start_tag != (int32_t)0xFFFF0001)
			break;
		
		// Otherwise, we have a subpop record; read in the rest of it
		p += sizeof(subpop_start_tag);
		
		subpop_id = *(slim_objectid_t *)p;
		p += sizeof(subpop_id);
		
		subpop_size = *(slim_popsize_t *)p;
		p += sizeof(subpop_size);
		
		sex_flag = *(int32_t *)p;
		p += sizeof(sex_flag);
		
		subpop_sex_ratio = *(double *)p;
		p += sizeof(subpop_sex_ratio);
		
		// Create the population population
		Subpopulation *new_subpop = population_.AddSubpopulation(subpop_id, subpop_size, subpop_sex_ratio);
		
		// define a new Eidos variable to refer to the new subpopulation
		EidosSymbolTableEntry &symbol_entry = new_subpop->SymbolTableEntry();
		
		if (p_interpreter && p_interpreter->SymbolTable().ContainsSymbol(symbol_entry.first))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): new subpopulation symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate();
		
		simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
	}
	
	if (p + sizeof(section_end_tag) > buf_end)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF after subpopulations." << eidos_terminate();
	else
	{
		section_end_tag = *(int32_t *)p;
		p += sizeof(section_end_tag);
		
		if (section_end_tag != (int32_t)0xFFFF0000)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): missing section end after subpopulations." << eidos_terminate();
	}
	
	// Read in the size of the mutation map, so we can allocate a vector rather than using std::map
	int32_t mutation_map_size;
	
	if (p + sizeof(mutation_map_size) > buf_end)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF at mutation map size." << eidos_terminate();
	else
	{
		mutation_map_size = *(int32_t *)p;
		p += sizeof(mutation_map_size);
	}
	
	// Mutations section
	std::unique_ptr<MutationIndex> raii_mutations(new MutationIndex[mutation_map_size]);
	MutationIndex *mutations = raii_mutations.get();
	
	if (!mutations)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): could not allocate mutations buffer." << eidos_terminate();
	
	while (true)
	{
		int32_t mutation_start_tag;
		slim_polymorphismid_t polymorphism_id;
		slim_mutationid_t mutation_id;					// Added in version 2
		slim_objectid_t mutation_type_id;
		slim_position_t position;
		slim_selcoeff_t selection_coeff;
		slim_selcoeff_t dominance_coeff;
		slim_objectid_t subpop_index;
		slim_generation_t generation;
		slim_refcount_t prevalence;
		
		// If there isn't enough buffer left to read a full mutation record, we assume we are done with this section
		int record_size = sizeof(mutation_start_tag) + sizeof(polymorphism_id) + sizeof(mutation_type_id) + sizeof(position) + sizeof(selection_coeff) + sizeof(dominance_coeff) + sizeof(subpop_index) + sizeof(generation) + sizeof(prevalence);
		
		if (file_version >= 2)
			record_size += sizeof(mutation_id);
		
		if (p + record_size > buf_end)
			break;
		
		// If the first int32_t is not a mutation start tag, then we are done with this section
		mutation_start_tag = *(int32_t *)p;
		if (mutation_start_tag != (int32_t)0xFFFF0002)
			break;
		
		// Otherwise, we have a mutation record; read in the rest of it
		p += sizeof(mutation_start_tag);
		
		polymorphism_id = *(slim_polymorphismid_t *)p;
		p += sizeof(polymorphism_id);
		
		if (file_version >= 2)
		{
			mutation_id = *(slim_mutationid_t *)p;
			p += sizeof(mutation_id);
		}
		else
		{
			mutation_id = polymorphism_id;		// when parsing version 1 output, we use the polymorphism id as the mutation id
		}
		
		mutation_type_id = *(slim_objectid_t *)p;
		p += sizeof(mutation_type_id);
		
		position = *(slim_position_t *)p;
		p += sizeof(position);
		
		selection_coeff = *(slim_selcoeff_t *)p;
		p += sizeof(selection_coeff);
		
		dominance_coeff = *(slim_selcoeff_t *)p;
		p += sizeof(dominance_coeff);
		
		subpop_index = *(slim_objectid_t *)p;
		p += sizeof(subpop_index);
		
		generation = *(slim_generation_t *)p;
		p += sizeof(generation);
		
		prevalence = *(slim_refcount_t *)p;
		p += sizeof(prevalence);
		
		// look up the mutation type from its index
		auto found_muttype_pair = mutation_types_.find(mutation_type_id);
		
		if (found_muttype_pair == mutation_types_.end()) 
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): mutation type m" << mutation_type_id << " has not been defined." << eidos_terminate();
		
		MutationType *mutation_type_ptr = found_muttype_pair->second;
		
		if (mutation_type_ptr->dominance_coeff_ != dominance_coeff)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): mutation type m" << mutation_type_id << " has dominance coefficient " << mutation_type_ptr->dominance_coeff_ << " that does not match the population file dominance coefficient of " << dominance_coeff << "." << eidos_terminate();
		
		// construct the new mutation; NOTE THAT THE STACKING POLICY IS NOT CHECKED HERE, AS THIS IS NOT CONSIDERED THE ADDITION OF A MUTATION!
		MutationIndex new_mut_index = SLiM_NewMutationFromBlock();
		
		new (gSLiM_Mutation_Block + new_mut_index) Mutation(mutation_id, mutation_type_ptr, position, selection_coeff, subpop_index, generation);
		
		// add it to our local map, so we can find it when making genomes, and to the population's mutation registry
		mutations[polymorphism_id] = new_mut_index;
		population_.mutation_registry_.emplace_back(new_mut_index);
		
		// all mutations seen here will be added to the simulation somewhere, so check and set pure_neutral_ and all_pure_neutral_DFE_
		if (selection_coeff != 0.0)
		{
			pure_neutral_ = false;
			mutation_type_ptr->all_pure_neutral_DFE_ = false;
		}
	}
	
	population_.cached_tally_genome_count_ = 0;
	
	if (p + sizeof(section_end_tag) > buf_end)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF after mutations." << eidos_terminate();
	else
	{
		section_end_tag = *(int32_t *)p;
		p += sizeof(section_end_tag);
		
		if (section_end_tag != (int32_t)0xFFFF0000)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): missing section end after mutations." << eidos_terminate();
	}
	
	// Genomes section
	Mutation *mut_block_ptr = gSLiM_Mutation_Block;
	bool use_16_bit = (mutation_map_size <= UINT16_MAX - 1);	// 0xFFFF is reserved as the start of our various tags
	std::unique_ptr<MutationIndex> raii_genomebuf(new MutationIndex[mutation_map_size]);	// allowing us to use emplace_back_bulk() for speed
	MutationIndex *genomebuf = raii_genomebuf.get();
	
	while (true)
	{
		slim_objectid_t subpop_id;
		slim_popsize_t genome_index;
		int32_t genome_type;
		int32_t total_mutations;
		
		// If there isn't enough buffer left to read a full genome record, we assume we are done with this section
		if (p + sizeof(genome_type) + sizeof(subpop_id) + sizeof(genome_index) + sizeof(total_mutations) > buf_end)
			break;
		
		// First check the first 32 bits to see if it is a section end tag
		genome_type = *(int32_t *)p;
		
		if (genome_type == (int32_t)0xFFFF0000)
			break;
		
		// If not, proceed with reading the genome entry
		p += sizeof(genome_type);
		
		subpop_id = *(slim_objectid_t *)p;
		p += sizeof(subpop_id);
		
		genome_index = *(slim_popsize_t *)p;
		p += sizeof(genome_index);
		
		// Look up the subpopulation
		auto subpop_pair = population_.find(subpop_id);
		
		if (subpop_pair == population_.end())
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): referenced subpopulation p" << subpop_id << " not defined." << eidos_terminate();
		
		Subpopulation &subpop = *subpop_pair->second;
		
		// Read in individual spatial position information.  Added in version 3.
		if (spatial_output_count && ((genome_index % 2) == 0))
		{
			// do another buffer length check
			if (p + spatial_output_count * sizeof(double) + sizeof(total_mutations) > buf_end)
			break;
			
			int individual_index = genome_index / 2;
			Individual &individual = subpop.parent_individuals_[individual_index];
			
			if (spatial_output_count >= 1)
			{
				individual.spatial_x_ = *(double *)p;
				p += sizeof(double);
			}
			if (spatial_output_count >= 2)
			{
				individual.spatial_y_ = *(double *)p;
				p += sizeof(double);
			}
			if (spatial_output_count >= 3)
			{
				individual.spatial_z_ = *(double *)p;
				p += sizeof(double);
			}
		}
		
		total_mutations = *(int32_t *)p;
		p += sizeof(total_mutations);
		
		// Look up the genome
		if ((genome_index < 0) || (genome_index > SLIM_MAX_SUBPOP_SIZE * 2))
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): genome index out of permitted range." << eidos_terminate();
		
		Genome &genome = subpop.parent_genomes_[genome_index];
		
		// Error-check the genome type
		if (genome_type != (int32_t)genome.Type())
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): genome type does not match the instantiated genome." << eidos_terminate();
		
		// Check the null genome state
		if (total_mutations == (int32_t)0xFFFF1000)
		{
			if (!genome.IsNull())
				EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): genome is specified as null, but the instantiated genome is non-null." << eidos_terminate();
		}
		else
		{
			if (genome.IsNull())
				EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): genome is specified as non-null, but the instantiated genome is null." << eidos_terminate();
			
			// Read in the mutation list
			int32_t mutcount = 0;
			
			if (use_16_bit)
			{
				// reading 16-bit mutation tags
				uint16_t mutation_id;
				
				if (p + sizeof(mutation_id) * total_mutations > buf_end)
					EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF while reading genome." << eidos_terminate();
				
				for (; mutcount < total_mutations; ++mutcount)
				{
					mutation_id = *(uint16_t *)p;
					p += sizeof(mutation_id);
					
					// Add mutation to genome
					if ((mutation_id < 0) || (mutation_id >= mutation_map_size)) 
						EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): mutation " << mutation_id << " has not been defined." << eidos_terminate();
					
					genomebuf[mutcount] = mutations[mutation_id];
				}
			}
			else
			{
				// reading 32-bit mutation tags
				int32_t mutation_id;
				
				if (p + sizeof(mutation_id) * total_mutations > buf_end)
					EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF while reading genome." << eidos_terminate();
				
				for (; mutcount < total_mutations; ++mutcount)
				{
					mutation_id = *(int32_t *)p;
					p += sizeof(mutation_id);
					
					// Add mutation to genome
					if ((mutation_id < 0) || (mutation_id >= mutation_map_size)) 
						EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): mutation " << mutation_id << " has not been defined." << eidos_terminate();
					
					genomebuf[mutcount] = mutations[mutation_id];
				}
			}
			
			int32_t mutrun_length_ = genome.mutrun_length_;
			int current_mutrun_index = -1;
			MutationRun *current_mutrun = nullptr;
			
			for (int mut_index = 0; mut_index < mutcount; ++mut_index)
			{
				MutationIndex mutation = genomebuf[mut_index];
				int mutrun_index = (mut_block_ptr + mutation)->position_ / mutrun_length_;
				
				if (mutrun_index != current_mutrun_index)
				{
					current_mutrun_index = mutrun_index;
					genome.WillModifyRun(current_mutrun_index);
					
					current_mutrun = genome.mutruns_[mutrun_index].get();
				}
				
				current_mutrun->emplace_back(mutation);
			}
		}
	}
	
	if (p + sizeof(section_end_tag) > buf_end)
		EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): unexpected EOF after genomes." << eidos_terminate();
	else
	{
		section_end_tag = *(int32_t *)p;
		p += sizeof(section_end_tag);
		
		if (section_end_tag != (int32_t)0xFFFF0000)
			EIDOS_TERMINATION << "ERROR (SLiMSim::InitializePopulationFromBinaryFile): missing section end after genomes." << eidos_terminate();
	}
	
	// It's a little unclear how we ought to clean up after ourselves, and this is a continuing source of bugs.  We could be loading
	// a new population in an early() event, in a late() event, or in between generations in SLiMgui using its Import Population command.
	// The safest avenue seems to be to just do all the bookkeeping we can think of: tally frequencies, calculate fitnesses, and
	// survey the population for SLiMgui.  This will lead to some of these actions being done at an unusual time in the generation cycle,
	// though, and will cause some things to be done unnecessarily (because they are not normally up-to-date at the current generation
	// cycle stage anyway) or done twice (which could be particularly problematic for fitness() callbacks).  Nevertheless, this seems
	// like the best policy, at least until shown otherwise...  BCH 11 June 2016
	
	// BCH 5 April 2017: Well, it has been shown otherwise.  Now that interactions have been added, fitness() callbacks often depend on
	// them, which means the interactions need to be evaluated, which means we can't evaluate fitness values yet; we need to give the
	// user's script a chance to evaluate the interactions.  This was always a problem, really; fitness() callbacks might have needed
	// some external state to be set up that would on the population state.  But now it is a glaring problem, and forces us to revise
	// our policy.  For backward compatibility, we will keep the old behavior if reading a file that is version 2 or earlier; a bit
	// weird, but probably nobody will ever even notice...
	
	// As of SLiM 2.1, we change the generation as a side effect of loading; otherwise we can't correctly update our state here!
	generation_ = file_generation;
	
	// Re-tally mutation references so we have accurate frequency counts for our new mutations
	population_.UniqueMutationRuns();
	population_.MaintainRegistry();
	
	if (file_version <= 2)
	{
		// Now that we have the info on everybody, update fitnesses so that we're ready to run the next generation
		// used to be generation + 1; removing that 18 Feb 2016 BCH
		
		nonneutral_change_counter_++;			// trigger unconditional nonneutral mutation caching inside UpdateFitness()
		last_nonneutral_regime_ = 3;			// this means "unpredictable callbacks", will trigger a recache next generation
		
		for (auto muttype_iter : mutation_types_)
			(muttype_iter.second)->subject_to_fitness_callback_ = true;	// we're not doing RecalculateFitness()'s work, so play it safe
		
		for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
		{
			slim_objectid_t subpop_id = subpop_pair.first;
			Subpopulation *subpop = subpop_pair.second;
			std::vector<SLiMEidosBlock*> fitness_callbacks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosFitnessCallback, -1, -1, subpop_id);
			std::vector<SLiMEidosBlock*> global_fitness_callbacks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback, -2, -1, subpop_id);
			
			subpop->UpdateFitness(fitness_callbacks, global_fitness_callbacks);
		}
		
#ifdef SLIMGUI
		// Let SLiMgui survey the population for mean fitness and such, if it is our target
		population_.SurveyPopulation();
#endif
	}
	
	return file_generation;
}

void SLiMSim::ValidateScriptBlockCaches(void)
{
	if (!script_block_types_cached_)
	{
		cached_early_events_.clear();
		cached_late_events_.clear();
		cached_initialize_callbacks_.clear();
		cached_fitness_callbacks_.clear();
		cached_fitnessglobal_callbacks_onegen_.clear();
		cached_fitnessglobal_callbacks_multigen_.clear();
		cached_interaction_callbacks_.clear();
		cached_matechoice_callbacks_.clear();
		cached_modifychild_callbacks_.clear();
		cached_recombination_callbacks_.clear();
		cached_userdef_functions_.clear();
		
		std::vector<SLiMEidosBlock*> &script_blocks = AllScriptBlocks();
		
		for (auto script_block : script_blocks)
		{
			switch (script_block->type_)
			{
				case SLiMEidosBlockType::SLiMEidosEventEarly:				cached_early_events_.push_back(script_block);				break;
				case SLiMEidosBlockType::SLiMEidosEventLate:				cached_late_events_.push_back(script_block);				break;
				case SLiMEidosBlockType::SLiMEidosInitializeCallback:		cached_initialize_callbacks_.push_back(script_block);		break;
				case SLiMEidosBlockType::SLiMEidosFitnessCallback:			cached_fitness_callbacks_.push_back(script_block);			break;
				case SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback:
				{
					// Global fitness callbacks are not order-dependent, so we don't have to preserve their order
					// of declaration the way we do with other types of callbacks.  This allows us to be very efficient
					// in how we look them up, which is good since sometimes we have a very large number of them.
					// We put those that are registered for just a single generation in a separate vector, which
					// we sort by generation, allowing us to look
					slim_generation_t start = script_block->start_generation_;
					slim_generation_t end = script_block->end_generation_;
					
					if (start == end)
					{
						cached_fitnessglobal_callbacks_onegen_.insert(std::pair<slim_generation_t, SLiMEidosBlock*>(start, script_block));
					}
					else
					{
						cached_fitnessglobal_callbacks_multigen_.push_back(script_block);
					}
					break;
				}
				case SLiMEidosBlockType::SLiMEidosInteractionCallback:		cached_interaction_callbacks_.push_back(script_block);		break;
				case SLiMEidosBlockType::SLiMEidosMateChoiceCallback:		cached_matechoice_callbacks_.push_back(script_block);		break;
				case SLiMEidosBlockType::SLiMEidosModifyChildCallback:		cached_modifychild_callbacks_.push_back(script_block);		break;
				case SLiMEidosBlockType::SLiMEidosRecombinationCallback:	cached_recombination_callbacks_.push_back(script_block);	break;
				case SLiMEidosBlockType::SLiMEidosUserDefinedFunction:		cached_userdef_functions_.push_back(script_block);			break;
			}
		}
		
		script_block_types_cached_ = true;
	}
}

std::vector<SLiMEidosBlock*> SLiMSim::ScriptBlocksMatching(slim_generation_t p_generation, SLiMEidosBlockType p_event_type, slim_objectid_t p_mutation_type_id, slim_objectid_t p_interaction_type_id, slim_objectid_t p_subpopulation_id)
{
	if (!script_block_types_cached_)
		ValidateScriptBlockCaches();
	
	std::vector<SLiMEidosBlock*> *block_list = nullptr;
	
	switch (p_event_type)
	{
		case SLiMEidosBlockType::SLiMEidosEventEarly:				block_list = &cached_early_events_;						break;
		case SLiMEidosBlockType::SLiMEidosEventLate:				block_list = &cached_late_events_;						break;
		case SLiMEidosBlockType::SLiMEidosInitializeCallback:		block_list = &cached_initialize_callbacks_;				break;
		case SLiMEidosBlockType::SLiMEidosFitnessCallback:			block_list = &cached_fitness_callbacks_;				break;
		case SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback:	block_list = &cached_fitnessglobal_callbacks_multigen_;	break;
		case SLiMEidosBlockType::SLiMEidosInteractionCallback:		block_list = &cached_interaction_callbacks_;			break;
		case SLiMEidosBlockType::SLiMEidosMateChoiceCallback:		block_list = &cached_matechoice_callbacks_;				break;
		case SLiMEidosBlockType::SLiMEidosModifyChildCallback:		block_list = &cached_modifychild_callbacks_;			break;
		case SLiMEidosBlockType::SLiMEidosRecombinationCallback:	block_list = &cached_recombination_callbacks_;			break;
		case SLiMEidosBlockType::SLiMEidosUserDefinedFunction:		block_list = &cached_userdef_functions_;				break;
	}
	
	std::vector<SLiMEidosBlock*> matches;
	
	for (SLiMEidosBlock *script_block : *block_list)
	{
		// check that the generation is in range
		if ((script_block->start_generation_ > p_generation) || (script_block->end_generation_ < p_generation))
			continue;
		
		// check that the script type matches (event, callback, etc.) - now guaranteed by the caching mechanism
		//if (script_block->type_ != p_event_type)
		//	continue;
		
		// check that the mutation type id matches, if requested
		// this is now a bit tricky, with the NULL mut-type option, indicated by -2.  The rules now are:
		//
		//    if -2 is requested, -2 callbacks are all you get
		//    if anything other than -2 is requested (including -1), -2 callbacks will not be returned
		//
		// so -2 callbacks are treated in a completely separate manner; they are never returned with other callbacks
		slim_objectid_t mutation_type_id = script_block->mutation_type_id_;
		
		if ((p_mutation_type_id == -2) && (mutation_type_id != -2))
			continue;
		if ((p_mutation_type_id != -2) && (mutation_type_id == -2))
			continue;
		
		if (p_mutation_type_id != -1)
		{
			if ((mutation_type_id != -1) && (p_mutation_type_id != mutation_type_id))
				continue;
		}
		
		// check that the interaction type id matches, if requested
		if (p_interaction_type_id != -1)
		{
			slim_objectid_t interaction_type_id = script_block->interaction_type_id_;
			
			if ((interaction_type_id != -1) && (p_interaction_type_id != interaction_type_id))
				continue;
		}
		
		// check that the subpopulation id matches, if requested
		if (p_subpopulation_id != -1)
		{
			slim_objectid_t subpopulation_id = script_block->subpopulation_id_;
			
			if ((subpopulation_id != -1) && (p_subpopulation_id != subpopulation_id))
				continue;
		}
		
		// OK, everything matches, so we want to return this script block
		matches.emplace_back(script_block);
	}
	
	// add in any single-generation global fitness callbacks
	if (p_event_type == SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback)
	{
		auto find_range = cached_fitnessglobal_callbacks_onegen_.equal_range(p_generation);
		auto find_start = find_range.first;
		auto find_end = find_range.second;
		
		for (auto block_iter = find_start; block_iter != find_end; ++block_iter)
		{
			SLiMEidosBlock *script_block = block_iter->second;
			
			// check that the subpopulation id matches, if requested
			if (p_subpopulation_id != -1)
			{
				slim_objectid_t subpopulation_id = script_block->subpopulation_id_;
				
				if ((subpopulation_id != -1) && (p_subpopulation_id != subpopulation_id))
					continue;
			}
			
			// OK, everything matches, so we want to return this script block
			matches.emplace_back(script_block);
		}
	}
	
	return matches;
}

std::vector<SLiMEidosBlock*> &SLiMSim::AllScriptBlocks()
{
	return script_blocks_;
}

void SLiMSim::OptimizeScriptBlock(SLiMEidosBlock *p_script_block)
{
	// The goal here is to look for specific structures in callbacks that we are able to optimize by short-circuiting
	// the callback interpretation entirely and replacing it with equivalent C++ code.  This is extremely messy, so
	// we're not going to do this for very many cases, but sometimes it is worth it.
	if (!p_script_block->has_cached_optimization_)
	{
		if (p_script_block->type_ == SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback)
		{
			const EidosASTNode *base_node = p_script_block->compound_statement_node_;
			
			if ((base_node->token_->token_type_ == EidosTokenType::kTokenLBrace) && (base_node->children_.size() == 1))
			{
				bool opt_dnorm1_candidate = true;
				const EidosASTNode *expr_node = base_node->children_[0];
				
				// if we have an intervening "return", jump down through it
				if ((expr_node->token_->token_type_ == EidosTokenType::kTokenReturn) && (expr_node->children_.size() == 1))
					expr_node = expr_node->children_[0];
				
				// parse an optional constant at the beginning, like 1.0 + ...
				double added_constant = NAN;
				
				if ((expr_node->token_->token_type_ == EidosTokenType::kTokenPlus) && (expr_node->children_.size() == 2))
				{
					const EidosASTNode *constant_node = expr_node->children_[0];
					const EidosASTNode *rhs_node = expr_node->children_[1];
					
					if (constant_node->HasCachedNumericValue())
					{
						added_constant = constant_node->CachedNumericValue();
						expr_node = rhs_node;
					}
					else
						opt_dnorm1_candidate = false;
				}
				else
				{
					added_constant = 0.0;
				}
				
				// parse an optional divisor at the end, ... / div
				double denominator = NAN;
				
				if ((expr_node->token_->token_type_ == EidosTokenType::kTokenDiv) && (expr_node->children_.size() == 2))
				{
					const EidosASTNode *numerator_node = expr_node->children_[0];
					const EidosASTNode *denominator_node = expr_node->children_[1];
					
					if (denominator_node->HasCachedNumericValue())
					{
						denominator = denominator_node->CachedNumericValue();
						expr_node = numerator_node;
					}
					else
						opt_dnorm1_candidate = false;
				}
				else
				{
					denominator = 1.0;
				}
				
				// parse the dnorm() function call
				if (opt_dnorm1_candidate && (expr_node->token_->token_type_ == EidosTokenType::kTokenLParen) && (expr_node->children_.size() >= 2))
				{
					const EidosASTNode *call_node = expr_node->children_[0];
					
					if ((call_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (call_node->token_->token_string_ == "dnorm"))
					{
						int child_count = (int)expr_node->children_.size();
						const EidosASTNode *x_node = expr_node->children_[1];
						const EidosASTNode *mean_node = (child_count >= 3) ? expr_node->children_[2] : nullptr;
						const EidosASTNode *sd_node = (child_count >= 4) ? expr_node->children_[3] : nullptr;
						double mean_value = 0.0, sd_value = 1.0;
						
						// resolve named arguments
						if (x_node && (x_node->token_->token_type_ == EidosTokenType::kTokenAssign) && (x_node->children_.size() == 2))
						{
							const EidosASTNode *name_node = x_node->children_[0];
							const EidosASTNode *value_node = x_node->children_[1];
							
							if ((name_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (name_node->token_->token_string_ == "x"))
								x_node = value_node;
							else
								opt_dnorm1_candidate = false;
						}
						if (mean_node && (mean_node->token_->token_type_ == EidosTokenType::kTokenAssign) && (mean_node->children_.size() == 2))
						{
							const EidosASTNode *name_node = mean_node->children_[0];
							const EidosASTNode *value_node = mean_node->children_[1];
							
							if ((name_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (name_node->token_->token_string_ == "mean"))
								mean_node = value_node;
							else
								opt_dnorm1_candidate = false;
						}
						if (sd_node && (sd_node->token_->token_type_ == EidosTokenType::kTokenAssign) && (sd_node->children_.size() == 2))
						{
							const EidosASTNode *name_node = sd_node->children_[0];
							const EidosASTNode *value_node = sd_node->children_[1];
							
							if ((name_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (name_node->token_->token_string_ == "sd"))
								sd_node = value_node;
							else
								opt_dnorm1_candidate = false;
						}
						
						// the mean and sd parameters of dnorm can be omitted in the below calls, but if they are given, get their values
						if (mean_node)
						{
							if (mean_node->HasCachedNumericValue())
								mean_value = mean_node->CachedNumericValue();
							else
								opt_dnorm1_candidate = false;
						}
						
						if (sd_node)
						{
							if (sd_node->HasCachedNumericValue())
								sd_value = sd_node->CachedNumericValue();
							else
								opt_dnorm1_candidate = false;
						}
						
						// parse the x argument to dnorm, which can take several different forms
						if (opt_dnorm1_candidate)
						{
							if ((x_node->token_->token_type_ == EidosTokenType::kTokenMinus) && (x_node->children_.size() == 2) && (mean_value == 0.0))
							{
								const EidosASTNode *lhs_node = x_node->children_[0];
								const EidosASTNode *rhs_node = x_node->children_[1];
								const EidosASTNode *dot_node = nullptr, *constant_node = nullptr;
								
								if (lhs_node->token_->token_type_ == EidosTokenType::kTokenDot)
								{
									dot_node = lhs_node;
									constant_node = rhs_node;
								}
								else if (rhs_node->token_->token_type_ == EidosTokenType::kTokenDot)
								{
									dot_node = rhs_node;
									constant_node = lhs_node;
								}
								
								if (dot_node && constant_node && (dot_node->children_.size() == 2) && constant_node->HasCachedNumericValue())
								{
									const EidosASTNode *var_node = dot_node->children_[0];
									const EidosASTNode *prop_node = dot_node->children_[1];
									
									mean_value = constant_node->CachedNumericValue();
									
									if ((var_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (var_node->token_->token_string_ == "individual")
										&& (prop_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (prop_node->token_->token_string_ == "tagF"))
									{
										// callback of the form { return D + dnorm(individual.tagF - A, 0.0, B) / C; }
										// callback of the form { return D + dnorm(individual.tagF - A, 0.0, B); }
										// callback of the form { D + dnorm(individual.tagF - A, 0.0, B) / C; }
										// callback of the form { D + dnorm(individual.tagF - A, 0.0, B); }
										// callback of the form { return D + dnorm(A - individual.tagF, 0.0, B) / C; }
										// callback of the form { return D + dnorm(A - individual.tagF, 0.0, B); }
										// callback of the form { D + dnorm(A - individual.tagF, 0.0, B) / C; }
										// callback of the form { D + dnorm(A - individual.tagF, 0.0, B); }
										p_script_block->has_cached_optimization_ = true;
										p_script_block->has_cached_opt_dnorm1_ = true;
										p_script_block->cached_opt_A_ = mean_value;
										p_script_block->cached_opt_B_ = sd_value;
										p_script_block->cached_opt_C_ = denominator;
										p_script_block->cached_opt_D_ = added_constant;
									}
								}
							}
							else if ((x_node->token_->token_type_ == EidosTokenType::kTokenDot) && (x_node->children_.size() == 2))
							{
								const EidosASTNode *var_node = x_node->children_[0];
								const EidosASTNode *prop_node = x_node->children_[1];
								
								if ((var_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (var_node->token_->token_string_ == "individual")
									&& (prop_node->token_->token_type_ == EidosTokenType::kTokenIdentifier) && (prop_node->token_->token_string_ == "tagF"))
								{
									// callback of the form { D + return dnorm(individual.tagF, A, B) / C; }
									// callback of the form { D + return dnorm(individual.tagF, A, B); }
									// callback of the form { D + dnorm(individual.tagF, A, B) / C; }
									// callback of the form { D + dnorm(individual.tagF, A, B); }
									p_script_block->has_cached_optimization_ = true;
									p_script_block->has_cached_opt_dnorm1_ = true;
									p_script_block->cached_opt_A_ = mean_value;
									p_script_block->cached_opt_B_ = sd_value;
									p_script_block->cached_opt_C_ = denominator;
									p_script_block->cached_opt_D_ = added_constant;
								}
							}
						}
					}
				}
			}
			
//			if (p_script_block->has_cached_optimization_ && p_script_block->has_cached_opt_dnorm1_)
//				std::cout << "optimized:" << std::endl << "   " << p_script_block->script_->String() << std::endl;
//			else
//				std::cout << "NOT OPTIMIZED:" << std::endl << "   " << p_script_block->script_->String() << std::endl;
		}
	}
}

void SLiMSim::AddScriptBlock(SLiMEidosBlock *p_script_block, EidosInterpreter *p_interpreter, const EidosToken *p_error_token)
{
	script_blocks_.emplace_back(p_script_block);
	
	p_script_block->TokenizeAndParse();	// can raise
	
	// The script block passed tokenization and parsing, so it is reasonably well-formed.  Now we check for cases we optimize.
	OptimizeScriptBlock(p_script_block);
	
	// Define the symbol for the script block, if any
	if (p_script_block->block_id_ != -1)
	{
		EidosSymbolTableEntry &symbol_entry = p_script_block->ScriptBlockSymbolTableEntry();
		EidosGlobalStringID symbol_id = symbol_entry.first;
		
		if ((simulation_constants_->ContainsSymbol(symbol_id)) || (p_interpreter && p_interpreter->SymbolTable().ContainsSymbol(symbol_id)))
			EIDOS_TERMINATION << "ERROR (SLiMSim::AddScriptBlock): script block symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate(p_error_token);
		
		simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
	}
	
	// Notify the various interested parties that the script blocks have changed
	last_script_block_gen_cached = false;
	script_block_types_cached_ = false;
	scripts_changed_ = true;
}

void SLiMSim::DeregisterScheduledScriptBlocks(void)
{
	// If we have blocks scheduled for deregistration, we sweep through and deregister them at the end of each stage of each generation.
	// This happens at a time when script blocks are not executing, so that we're guaranteed not to leave hanging pointers that could
	// cause a crash; it also guarantees that script blocks are applied consistently across each generation stage.  A single block
	// might be scheduled for deregistration more than once, but should only occur in script_blocks_ once, so we have to be careful
	// with our deallocations here; we deallocate a block only when we find it in script_blocks_.
	for (SLiMEidosBlock *block_to_dereg : scheduled_deregistrations_)
	{
		auto script_block_position = std::find(script_blocks_.begin(), script_blocks_.end(), block_to_dereg);
		
		if (script_block_position != script_blocks_.end())
		{
			// Remove the symbol for it first
			if (block_to_dereg->block_id_ != -1)
				simulation_constants_->RemoveConstantForSymbol(block_to_dereg->ScriptBlockSymbolTableEntry().first);
			
			// Then remove it from our script block list and deallocate it
			script_blocks_.erase(script_block_position);
			last_script_block_gen_cached = false;
			script_block_types_cached_ = false;
			scripts_changed_ = true;
			delete block_to_dereg;
		}
	}
}

void SLiMSim::DeregisterScheduledInteractionBlocks(void)
{
	// Identical to DeregisterScheduledScriptBlocks() above, but for the interaction() dereg list; see deregisterScriptBlock()
	for (SLiMEidosBlock *block_to_dereg : scheduled_interaction_deregs_)
	{
		auto script_block_position = std::find(script_blocks_.begin(), script_blocks_.end(), block_to_dereg);
		
		if (script_block_position != script_blocks_.end())
		{
			// Remove the symbol for it first
			if (block_to_dereg->block_id_ != -1)
				simulation_constants_->RemoveConstantForSymbol(block_to_dereg->ScriptBlockSymbolTableEntry().first);
			
			// Then remove it from our script block list and deallocate it
			script_blocks_.erase(script_block_position);
			last_script_block_gen_cached = false;
			script_block_types_cached_ = false;
			scripts_changed_ = true;
			delete block_to_dereg;
		}
	}
}

void SLiMSim::ExecuteFunctionDefinitionBlock(SLiMEidosBlock *p_script_block)
{
	EidosSymbolTable callback_symbols(EidosSymbolTableType::kContextConstantsTable, &SymbolTable());
	EidosSymbolTable client_symbols(EidosSymbolTableType::kVariablesTable, &callback_symbols);
	
	EidosInterpreter interpreter(p_script_block->root_node_->children_[0], client_symbols, simulation_functions_, this);
	
	try
	{
		// Interpret the script; the result from the interpretation is not used for anything
		EidosValue_SP result = interpreter.EvaluateInternalBlock(p_script_block->script_);
		
		// Output generated by the interpreter goes to our output stream
		SLIM_OUTSTREAM << interpreter.ExecutionOutput();
	}
	catch (...)
	{
		// Emit final output even on a throw, so that stop() messages and such get printed
		SLIM_OUTSTREAM << interpreter.ExecutionOutput();
		
		throw;
	}
}

void SLiMSim::RunInitializeCallbacks(void)
{
	// zero out the initialization check counts
	num_interaction_types_ = 0;
	num_mutation_types_ = 0;
	num_mutation_rates_ = 0;
	num_genomic_element_types_ = 0;
	num_genomic_elements_ = 0;
	num_recombination_rates_ = 0;
	num_gene_conversions_ = 0;
	num_sex_declarations_ = 0;
	num_options_declarations_ = 0;
	
	if (DEBUG_INPUT)
		SLIM_OUTSTREAM << "// RunInitializeCallbacks():" << endl;
	
	// execute user-defined function blocks first; no need to profile this, it's just the definitions not the executions
	std::vector<SLiMEidosBlock*> function_blocks = ScriptBlocksMatching(-1, SLiMEidosBlockType::SLiMEidosUserDefinedFunction, -1, -1, -1);
	
	for (auto script_block : function_blocks)
		ExecuteFunctionDefinitionBlock(script_block);
	
	// execute initialize() callbacks, which should always have a generation of 0 set
	std::vector<SLiMEidosBlock*> init_blocks = ScriptBlocksMatching(0, SLiMEidosBlockType::SLiMEidosInitializeCallback, -1, -1, -1);
	
	for (auto script_block : init_blocks)
	{
		if (script_block->active_)
		{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_START();
#endif
			
			population_.ExecuteScript(script_block, generation_, chromosome_);
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosInitializeCallback)]);
#endif
		}
	}
	
	DeregisterScheduledScriptBlocks();
	
	// we're done with the initialization generation, so remove the zero-gen functions
	RemoveZeroGenerationFunctionsFromMap(simulation_functions_);
	
	// check for complete initialization
	if (num_mutation_rates_ == 0)
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): At least one mutation rate interval must be defined in an initialize() callback with initializeMutationRate()." << eidos_terminate();
	
	if (num_mutation_types_ == 0)
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): At least one mutation type must be defined in an initialize() callback with initializeMutationType()." << eidos_terminate();
	
	if (num_genomic_element_types_ == 0)
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): At least one genomic element type must be defined in an initialize() callback with initializeGenomicElementType()." << eidos_terminate();
	
	if (num_genomic_elements_ == 0)
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): At least one genomic element must be defined in an initialize() callback with initializeGenomicElement()." << eidos_terminate();
	
	if (num_recombination_rates_ == 0)
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): At least one recombination rate interval must be defined in an initialize() callback with initializeRecombinationRate()." << eidos_terminate();
	
	if ((chromosome_.recombination_rates_H_.size() != 0) && ((chromosome_.recombination_rates_M_.size() != 0) || (chromosome_.recombination_rates_F_.size() != 0)))
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): Cannot define both sex-specific and sex-nonspecific recombination rates." << eidos_terminate();
	
	if (((chromosome_.recombination_rates_M_.size() == 0) && (chromosome_.recombination_rates_F_.size() != 0)) ||
		((chromosome_.recombination_rates_M_.size() != 0) && (chromosome_.recombination_rates_F_.size() == 0)))
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): Both sex-specific recombination rates must be defined, not just one (but one may be defined as zero)." << eidos_terminate();
	
	CheckMutationStackPolicy();
	
	time_start_ = FirstGeneration();	// SLIM_MAX_GENERATION if it can't find a first block
	
	if (time_start_ == SLIM_MAX_GENERATION)
		EIDOS_TERMINATION << "ERROR (SLiMSim::RunInitializeCallbacks): No Eidos event found to start the simulation." << eidos_terminate();
	
	// start at the beginning
	generation_ = time_start_;
	
	// set up the "sim" symbol now that initialization is complete
	simulation_constants_->InitializeConstantSymbolEntry(SymbolTableEntry());
	
	// initialize chromosome
	chromosome_.InitializeDraws();
	chromosome_.ChooseMutationRunLayout(preferred_mutrun_count_);
	
	// kick off mutation run experiments, if needed
	InitiateMutationRunExperiments();
	
	// emit our start log
	SLIM_OUTSTREAM << "\n// Starting run at generation <start>:\n" << time_start_ << " " << "\n" << std::endl;
}

void SLiMSim::InitiateMutationRunExperiments(void)
{
	if (preferred_mutrun_count_ != 0)
	{
		// If the user supplied a count, go with that and don't run experiments
		x_experiments_enabled_ = false;
		
		if (SLiM_verbose_output)
		{
			SLIM_OUTSTREAM << std::endl;
			SLIM_OUTSTREAM << "// Mutation run experiments disabled since a mutation run count was supplied" << std::endl;
		}
		
		return;
	}
	if (chromosome_.mutrun_length_ <= SLIM_MUTRUN_MAXIMUM_COUNT)
	{
		// If the chromosome length is too short, go with that and don't run experiments;
		// we want to guarantee that with SLIM_MUTRUN_MAXIMUM_COUNT runs each mutrun is at
		// least one mutation in length, so the code doesn't break down
		x_experiments_enabled_ = false;
		
		if (SLiM_verbose_output)
		{
			SLIM_OUTSTREAM << std::endl;
			SLIM_OUTSTREAM << "// Mutation run experiments disabled since the chromosome is very short" << std::endl;
		}
		
		return;
	}
	
	x_experiments_enabled_ = true;
	
	x_current_mutcount_ = chromosome_.mutrun_count_;
	x_current_runtimes_ = (double *)malloc(SLIM_MUTRUN_EXPERIMENT_LENGTH * sizeof(double));
	x_current_buflen_ = 0;
	
	x_previous_mutcount_ = 0;			// marks that no previous experiment has been done
	x_previous_runtimes_ = (double *)malloc(SLIM_MUTRUN_EXPERIMENT_LENGTH * sizeof(double));
	x_previous_buflen_ = 0;
	
	x_continuing_trend = false;
	
	x_stasis_limit_ = 5;				// once we reach stasis, we will conduct 5 stasis experiments before exploring again
	x_stasis_alpha_ = 0.01;				// initially, we use an alpha of 0.01 to break out of stasis due to a change in mean
	x_prev1_stasis_mutcount_ = 0;		// we have never reached stasis before, so we have no memory of it
	x_prev2_stasis_mutcount_ = 0;		// we have never reached stasis before, so we have no memory of it
	
	if (SLiM_verbose_output)
	{
		SLIM_OUTSTREAM << std::endl;
		SLIM_OUTSTREAM << "// Mutation run experiments started" << std::endl;
	}
}

void SLiMSim::TransitionToNewExperimentAgainstCurrentExperiment(int32_t p_new_mutrun_count)
{
	// Save off the old experiment
	x_previous_mutcount_ = x_current_mutcount_;
	std::swap(x_current_runtimes_, x_previous_runtimes_);
	x_previous_buflen_ = x_current_buflen_;
	
	// Set up the next experiment
	x_current_mutcount_ = p_new_mutrun_count;
	x_current_buflen_ = 0;
}

void SLiMSim::TransitionToNewExperimentAgainstPreviousExperiment(int32_t p_new_mutrun_count)
{
	// Set up the next experiment
	x_current_mutcount_ = p_new_mutrun_count;
	x_current_buflen_ = 0;
}

void SLiMSim::EnterStasisForMutationRunExperiments(void)
{
	if ((x_current_mutcount_ == x_prev1_stasis_mutcount_) || (x_current_mutcount_ == x_prev2_stasis_mutcount_))
	{
		// One of our recent trips to stasis was at the same count, so we broke stasis incorrectly; get stricter.
		// The purpose for keeping two previous counts is to detect when we are ping-ponging between two values
		// that produce virtually identical performance; we want to detect that and just settle on one of them.
		x_stasis_alpha_ *= 0.5;
		x_stasis_limit_ *= 2;
		
#if MUTRUN_EXPERIMENT_OUTPUT
		if (SLiM_verbose_output)
			SLIM_OUTSTREAM << "// Remembered previous stasis at " << x_current_mutcount_ << ", strengthening stasis criteria" << std::endl;
#endif
	}
	else
	{
		// Our previous trips to stasis were at a different number of mutation runs, so reset our stasis parameters
		x_stasis_limit_ = 5;
		x_stasis_alpha_ = 0.01;
		
#if MUTRUN_EXPERIMENT_OUTPUT
		if (SLiM_verbose_output)
			SLIM_OUTSTREAM << "// No memory of previous stasis at " << x_current_mutcount_ << ", resetting stasis criteria" << std::endl;
#endif
	}
	
	x_stasis_counter_ = 1;
	x_continuing_trend = false;
	
	// Preserve a memory of the last two *different* mutcounts we entered stasis on.  Only forget the old value
	// in x_prev2_stasis_mutcount_ if x_prev1_stasis_mutcount_ is about to get a new and different value.
	// This makes the anti-ping-pong mechanism described above effective even if we ping-pong irregularly.
	if (x_prev1_stasis_mutcount_ != x_current_mutcount_)
		x_prev2_stasis_mutcount_ = x_prev1_stasis_mutcount_;
	x_prev1_stasis_mutcount_ = x_current_mutcount_;
	
#if MUTRUN_EXPERIMENT_OUTPUT
	if (SLiM_verbose_output)
		SLIM_OUTSTREAM << "// ****** ENTERING STASIS AT " << x_current_mutcount_ << " : x_stasis_limit_ = " << x_stasis_limit_ << ", x_stasis_alpha_ = " << x_stasis_alpha_ << std::endl;
#endif
}

void SLiMSim::MaintainMutationRunExperiments(double p_last_gen_runtime)
{
	// Log the last generation time into our buffer
	if (x_current_buflen_ >= SLIM_MUTRUN_EXPERIMENT_LENGTH)
		EIDOS_TERMINATION << "ERROR (SLiMSim::MaintainMutationRunExperiments): Buffer overrun, failure to reset after completion of an experiment." << eidos_terminate();
	
	x_current_runtimes_[x_current_buflen_] = p_last_gen_runtime;
	
	// Remember the history of the mutation run count
	x_mutcount_history_.push_back(x_current_mutcount_);
	
	// If the current experiment is not over, continue running it
	++x_current_buflen_;
	
	double current_mean = 0.0, previous_mean = 0.0, p = 0.0;
	
	if ((x_current_buflen_ == 10) && (x_current_mutcount_ != x_previous_mutcount_) && (x_previous_mutcount_ != 0))
	{
		// We want to be able to cut an experiment short if it is clearly a disaster.  So if we're not in stasis, and
		// we've run for 10 generations, and the experiment mean is already different from the baseline at alpha 0.01,
		// and the experiment mean is worse than the baseline mean (if it is better, we want to continue collecting),
		// let's short-circuit the rest of the experiment and bail – like early termination of a medical trial.
		p = Eidos_TTest_TwoSampleWelch(x_current_runtimes_, x_current_buflen_, x_previous_runtimes_, x_previous_buflen_, &current_mean, &previous_mean);
		
		if ((p < 0.01) && (current_mean > previous_mean))
		{
#if MUTRUN_EXPERIMENT_OUTPUT
			if (SLiM_verbose_output)
			{
				SLIM_OUTSTREAM << std::endl;
				SLIM_OUTSTREAM << "// " << generation_ << " : Early t-test yielded HIGHLY SIGNIFICANT p of " << p << " with negative results; terminating early." << std::endl;
			}
#endif
			
			goto early_ttest_passed;
		}
#if MUTRUN_EXPERIMENT_OUTPUT
		else if (SLiM_verbose_output)
		{
			if (p >= 0.01)
			{
				SLIM_OUTSTREAM << std::endl;
				SLIM_OUTSTREAM << "// " << generation_ << " : Early t-test yielded not highly significant p of " << p << "; continuing." << std::endl;
			}
			else if (current_mean > previous_mean)
			{
				SLIM_OUTSTREAM << std::endl;
				SLIM_OUTSTREAM << "// " << generation_ << " : Early t-test yielded highly significant p of " << p << " with positive results; continuing data collection." << std::endl;
			}
		}
#endif
	}
	
	if (x_current_buflen_ < SLIM_MUTRUN_EXPERIMENT_LENGTH)
		return;
	
	if (x_previous_mutcount_ == 0)
	{
		// FINISHED OUR FIRST EXPERIMENT; move on to the next experiment, which is always double the number of mutruns
#if MUTRUN_EXPERIMENT_OUTPUT
		if (SLiM_verbose_output)
		{
			SLIM_OUTSTREAM << std::endl;
			SLIM_OUTSTREAM << "// ** " << generation_ << " : First mutation run experiment completed with mutrun count " << x_current_mutcount_ << "; will now try " << (x_current_mutcount_ * 2) << std::endl;
		}
#endif
		
		TransitionToNewExperimentAgainstCurrentExperiment(x_current_mutcount_ * 2);
	}
	else
	{
		// If we've just finished the second stasis experiment, run another stasis experiment before trying to draw any
		// conclusions.  We often enter stasis with one generation's worth of data that was actually collected quite a
		// while ago, because we did exploration in both directions first.  This can lead to breaking out of stasis
		// immediately after entering, because we're comparing apples and oranges.  So we avoid doing that here.
		if ((x_stasis_counter_ <= 1) && (x_current_mutcount_ == x_previous_mutcount_))
		{
			TransitionToNewExperimentAgainstCurrentExperiment(x_current_mutcount_);
			++x_stasis_counter_;
			
#if MUTRUN_EXPERIMENT_OUTPUT
			if (SLiM_verbose_output)
			{
				SLIM_OUTSTREAM << std::endl;
				SLIM_OUTSTREAM << "// " << generation_ << " : Mutation run experiment completed (second stasis generation, no tests conducted)" << std::endl;
			}
#endif
			
			return;
		}
		
		// Otherwise, get a result from a t-test and decide what to do
		p = Eidos_TTest_TwoSampleWelch(x_current_runtimes_, x_current_buflen_, x_previous_runtimes_, x_previous_buflen_, &current_mean, &previous_mean);
		
	early_ttest_passed:
		
#if MUTRUN_EXPERIMENT_OUTPUT
		if (SLiM_verbose_output)
		{
			SLIM_OUTSTREAM << std::endl;
			SLIM_OUTSTREAM << "// " << generation_ << " : Mutation run experiment completed:" << std::endl;
			SLIM_OUTSTREAM << "//    mean == " << current_mean << " for " << x_current_mutcount_ << " mutruns (" << x_current_buflen_ << " data points)" << std::endl;
			SLIM_OUTSTREAM << "//    mean == " << previous_mean << " for " << x_previous_mutcount_ << " mutruns (" << x_previous_buflen_ << " data points)" << std::endl;
		}
#endif
		
		if (x_current_mutcount_ == x_previous_mutcount_)	// are we in stasis?
		{
			//
			// FINISHED A STASIS EXPERIMENT; unless we have changed at alpha = 0.01 we stay put
			//
			bool means_different_stasis = (p < x_stasis_alpha_);
			
#if MUTRUN_EXPERIMENT_OUTPUT
			if (SLiM_verbose_output)
				SLIM_OUTSTREAM << "//    p == " << p << " : " << (means_different_stasis ? "SIGNIFICANT DIFFERENCE" : "no significant difference") << " at stasis alpha " << x_stasis_alpha_ << std::endl;
#endif
			
			if (means_different_stasis)
			{
				// OK, it looks like something has changed about our scenario, so we should come out of stasis and re-test.
				// We don't have any information about the new state of affairs, so we have no directional preference.
				// Let's try a larger number of mutation runs first, since genomes tend to fill up, unless we're at the max.
				if (x_current_mutcount_ >= SLIM_MUTRUN_MAXIMUM_COUNT)
					TransitionToNewExperimentAgainstCurrentExperiment(x_current_mutcount_ / 2);
				else
					TransitionToNewExperimentAgainstCurrentExperiment(x_current_mutcount_ * 2);
				
#if MUTRUN_EXPERIMENT_OUTPUT
				if (SLiM_verbose_output)
					SLIM_OUTSTREAM << "// ** " << generation_ << " : Stasis mean changed, EXITING STASIS and trying new mutcount of " << x_current_mutcount_ << std::endl;
#endif
			}
			else
			{
				// We seem to be in a constant scenario.  Increment our stasis counter and see if we have reached our stasis limit
				if (++x_stasis_counter_ >= x_stasis_limit_)
				{
					// We reached the stasis limit, so we will try an experiment even though we don't seem to have changed;
					// as before, we try more mutation runs first, since increasing genetic complexity is typical
					if (x_current_mutcount_ >= SLIM_MUTRUN_MAXIMUM_COUNT)
						TransitionToNewExperimentAgainstCurrentExperiment(x_current_mutcount_ / 2);
					else
						TransitionToNewExperimentAgainstCurrentExperiment(x_current_mutcount_ * 2);
					
#if MUTRUN_EXPERIMENT_OUTPUT
					if (SLiM_verbose_output)
						SLIM_OUTSTREAM << "// ** " << generation_ << " : Stasis limit reached, EXITING STASIS and trying new mutcount of " << x_current_mutcount_ << std::endl;
#endif
				}
				else
				{
					// We have not yet reached the stasis limit, so run another stasis experiment.
					// In this case we don't do a transition; we want to continue comparing against the original experiment
					// data so that if stasis slowly drift away from us, we eventually detect that as a change in stasis.
					x_current_buflen_ = 0;
					
#if MUTRUN_EXPERIMENT_OUTPUT
					if (SLiM_verbose_output)
						SLIM_OUTSTREAM << "//    " << generation_ << " : Stasis limit not reached (" << x_stasis_counter_ << " of " << x_stasis_limit_ << "), running another stasis experiment at " << x_current_mutcount_ << std::endl;
#endif
				}
			}
		}
		else
		{
			//
			// FINISHED A NON-STASIS EXPERIMENT; trying a move toward more/fewer mutruns
			//
			double alpha = 0.05;
			bool means_different_05 = (p < alpha);
			
#if MUTRUN_EXPERIMENT_OUTPUT
			if (SLiM_verbose_output)
				SLIM_OUTSTREAM << "//    p == " << p << " : " << (means_different_05 ? "SIGNIFICANT DIFFERENCE" : "no significant difference") << " at alpha " << alpha << std::endl;
#endif
			
			int32_t trend_next = (x_current_mutcount_ < x_previous_mutcount_) ? (x_current_mutcount_ / 2) : (x_current_mutcount_ * 2);
			int32_t trend_limit = (x_current_mutcount_ < x_previous_mutcount_) ? 1 : SLIM_MUTRUN_MAXIMUM_COUNT;
			
			if ((current_mean < previous_mean) || (!means_different_05 && (x_current_mutcount_ < x_previous_mutcount_)))
			{
				// We enter this case under two different conditions.  The first is that the new mean is better
				// than the old mean; whether that is significant or not, we want to continue in the same direction
				// with a new experiment, which is what we do here.  The other case is if the new mean is worse
				// than the old mean, but the difference is non-significant *and* we're trending toward fewer
				// mutation runs.  We treat that the same way: continue with a new experiment in the same direction.
				// But if the new mean is worse that the old mean and we're trending toward more mutation runs,
				// we do NOT follow this case, because an inconclusive but negative increasing trend pushes up our
				// peak memory usage and can be quite inefficient, and usually we just jump back down anyway.
				if (x_current_mutcount_ == trend_limit)
				{
					if (current_mean < previous_mean)
					{
						// Can't go beyond the trend limit (1 or SLIM_MUTRUN_MAXIMUM_COUNT), so we're done; ****** ENTER STASIS
						// We keep the current experiment as the first stasis experiment.
						TransitionToNewExperimentAgainstCurrentExperiment(x_current_mutcount_);
						
#if MUTRUN_EXPERIMENT_OUTPUT
						if (SLiM_verbose_output)
							SLIM_OUTSTREAM << "// ****** " << generation_ << " : Experiment " << (means_different_05 ? "successful" : "inconclusive but positive") << " at " << x_previous_mutcount_ << ", nowhere left to go; entering stasis at " << x_current_mutcount_ << "." << std::endl;
#endif
						
						EnterStasisForMutationRunExperiments();
					}
					else
					{
						// The means are not significantly different, and the current experiment is worse than the
						// previous one, and we can't go beyond the trend limit, so we're done; ****** ENTER STASIS
						// We keep the previous experiment as the first stasis experiment.
						TransitionToNewExperimentAgainstPreviousExperiment(x_previous_mutcount_);
						
#if MUTRUN_EXPERIMENT_OUTPUT
						if (SLiM_verbose_output)
							SLIM_OUTSTREAM << "// ****** " << generation_ << " : Experiment " << (means_different_05 ? "failed" : "inconclusive but negative") << " at " << x_previous_mutcount_ << ", nowhere left to go; entering stasis at " << x_current_mutcount_ << "." << std::endl;
#endif
						
						EnterStasisForMutationRunExperiments();
					}
				}
				else
				{
					if (current_mean < previous_mean)
					{
						// Even if the difference is not significant, we appear to be moving in a beneficial direction,
						// so we will run the next experiment against the current experiment's results
#if MUTRUN_EXPERIMENT_OUTPUT
						if (SLiM_verbose_output)
							SLIM_OUTSTREAM << "// ** " << generation_ << " : Experiment " << (means_different_05 ? "successful" : "inconclusive but positive") << " at " << x_current_mutcount_ << " (against " << x_previous_mutcount_ << "), continuing trend with " << trend_next << " (against " << x_current_mutcount_ << ")" << std::endl;
#endif
						
						TransitionToNewExperimentAgainstCurrentExperiment(trend_next);
						x_continuing_trend = true;
					}
					else
					{
						// The difference is not significant, but we might be moving in a bad direction, and a series
						// of such moves, each non-significant against the previous experiment, can lead us way down
						// the garden path.  To make sure that doesn't happen, we run successive inconclusive experiments
						// against whichever preceding experiment had the lowest mean.
#if MUTRUN_EXPERIMENT_OUTPUT
						if (SLiM_verbose_output)
							SLIM_OUTSTREAM << "// ** " << generation_ << " : Experiment inconclusive but negative at " << x_current_mutcount_ << " (against " << x_previous_mutcount_ << "), checking " << trend_next << " (against " << x_previous_mutcount_ << ")" << std::endl;
#endif
						
						TransitionToNewExperimentAgainstPreviousExperiment(trend_next);
					}
				}
			}
			else
			{
				// The old mean was better, and either the difference is significant or the trend is toward more mutation
				// runs, so we want to give up on this trend and go back
				if (x_continuing_trend)
				{
					// We already tried a step on the opposite side of the old position, so the old position appears ideal; ****** ENTER STASIS.
					// We throw away the current, failed experiment and keep the last experiment at the previous position as the first stasis experiment.
					TransitionToNewExperimentAgainstPreviousExperiment(x_previous_mutcount_);
					
#if MUTRUN_EXPERIMENT_OUTPUT
					if (SLiM_verbose_output)
						SLIM_OUTSTREAM << "// ****** " << generation_ << " : Experiment failed, already tried opposite side, so " << x_current_mutcount_ << " appears optimal; entering stasis at " << x_current_mutcount_ << "." << std::endl;
#endif
					
					EnterStasisForMutationRunExperiments();
				}
				else
				{
					// We have not tried a step on the opposite side of the old position; let's return to our old position,
					// which we know is better than the position we just ran an experiment at, and then advance onward to
					// run an experiment at the next position in that reversed trend direction.
					
					if ((x_previous_mutcount_ == 1) || (x_previous_mutcount_ == SLIM_MUTRUN_MAXIMUM_COUNT))
					{
						// can't jump over the previous mutcount, so we enter stasis at it
						TransitionToNewExperimentAgainstPreviousExperiment(x_previous_mutcount_);
						
#if MUTRUN_EXPERIMENT_OUTPUT
						if (SLiM_verbose_output)
							SLIM_OUTSTREAM << "// ****** " << generation_ << " : Experiment failed, opposite side blocked so " << x_current_mutcount_ << " appears optimal; entering stasis at " << x_current_mutcount_ << "." << std::endl;
#endif
						
						EnterStasisForMutationRunExperiments();
					}
					else
					{
						int32_t new_mutcount = ((x_current_mutcount_ > x_previous_mutcount_) ? (x_previous_mutcount_ / 2) : (x_previous_mutcount_ * 2));
						
#if MUTRUN_EXPERIMENT_OUTPUT
						if (SLiM_verbose_output)
							SLIM_OUTSTREAM << "// ** " << generation_ << " : Experiment failed at " << x_current_mutcount_ << ", opposite side untried, reversing trend back to " << new_mutcount << " (against " << x_previous_mutcount_ << ")" << std::endl;
#endif
						
						TransitionToNewExperimentAgainstPreviousExperiment(new_mutcount);
						x_continuing_trend = true;
					}
				}
			}
		}
	}
	
	// Promulgate the new mutation run count
	if (x_current_mutcount_ != chromosome_.mutrun_count_)
	{
		// Fix all genomes.  We could do this by brute force, by making completely new mutation runs for every
		// existing genome and then calling Population::UniqueMutationRuns(), but that would be inefficient,
		// and would also cause a huge memory usage spike.  Instead, we want to preserve existing redundancy.
		
		while (x_current_mutcount_ > chromosome_.mutrun_count_)
		{
#if MUTRUN_EXPERIMENT_OUTPUT
			clock_t start_clock = clock();
#endif
			
			// We are splitting existing runs in two, so make a map from old mutrun index to new pair of
			// mutrun indices; every time we encounter the same old index we will substitute the same pair.
			population_.SplitMutationRuns(chromosome_.mutrun_count_ * 2);
			
			// Fix the chromosome values
			chromosome_.mutrun_count_ *= 2;
			chromosome_.mutrun_length_ /= 2;
			
#if MUTRUN_EXPERIMENT_OUTPUT
			if (SLiM_verbose_output)
				SLIM_OUTSTREAM << "// ++ Splitting to achieve new mutation run count of " << chromosome_.mutrun_count_ << " took " << ((clock() - start_clock) / (double)CLOCKS_PER_SEC) << " seconds" << std::endl;
#endif
		}
		
		while (x_current_mutcount_ < chromosome_.mutrun_count_)
		{
#if MUTRUN_EXPERIMENT_OUTPUT
			clock_t start_clock = clock();
#endif
			
			// We are joining existing runs together, so make a map from old mutrun index pairs to a new
			// index; every time we encounter the same pair of indices we will substitute the same index.
			population_.JoinMutationRuns(chromosome_.mutrun_count_ / 2);
			
			// Fix the chromosome values
			chromosome_.mutrun_count_ /= 2;
			chromosome_.mutrun_length_ *= 2;
			
#if MUTRUN_EXPERIMENT_OUTPUT
			if (SLiM_verbose_output)
				SLIM_OUTSTREAM << "// ++ Joining to achieve new mutation run count of " << chromosome_.mutrun_count_ << " took " << ((clock() - start_clock) / (double)CLOCKS_PER_SEC) << " seconds" << std::endl;
#endif
		}
		
		if (chromosome_.mutrun_count_ != x_current_mutcount_)
			EIDOS_TERMINATION << "ERROR (SLiMSim::MaintainMutationRunExperiments): Failed to transition to new mutation run count" << x_current_mutcount_ << "." << eidos_terminate();
	}
}

#if defined(SLIMGUI) && (SLIMPROFILING == 1)
// PROFILING
#if SLIM_USE_NONNEUTRAL_CACHES
void SLiMSim::CollectSLiMguiMutationProfileInfo(void)
{
	// maintain our history of the number of mutruns per genome and the nonneutral regime
	profile_mutcount_history_.push_back(chromosome_.mutrun_count_);
	profile_nonneutral_regime_history_.push_back(last_nonneutral_regime_);
	
	// track the maximum number of mutations in existence at one time
	profile_max_mutation_index_ = std::max(profile_max_mutation_index_, (int64_t)(population_.mutation_registry_.size()));
	
	// tally up the number of mutation runs, mutation usage metrics, etc.
	int64_t operation_id = ++gSLiM_MutationRun_OperationID;
	
	for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
	{
		Subpopulation *subpop = subpop_pair.second;
		std::vector<Genome> &subpop_genomes = subpop->parent_genomes_;
		
		for (Genome &genome : subpop_genomes)
		{
			MutationRun_SP *mutruns = genome.mutruns_;
			int32_t mutrun_count = genome.mutrun_count_;
			
			profile_mutrun_total_usage_ += mutrun_count;
			
			for (int32_t mutrun_index = 0; mutrun_index < mutrun_count; ++mutrun_index)
			{
				MutationRun *mutrun = mutruns[mutrun_index].get();
				
				if (mutrun)
				{
					if (mutrun->operation_id_ != operation_id)
					{
						mutrun->operation_id_ = operation_id;
						profile_unique_mutrun_total_++;
					}
					
					// tally the total and nonneutral mutations
					mutrun->tally_nonneutral_mutations(&profile_mutation_total_usage_, &profile_nonneutral_mutation_total_, &profile_mutrun_nonneutral_recache_total_);
				}
			}
		}
	}
}
#endif
#endif

slim_generation_t SLiMSim::FirstGeneration(void)
{
	slim_generation_t first_gen = SLIM_MAX_GENERATION;
	std::vector<SLiMEidosBlock*> &script_blocks = AllScriptBlocks();
	
	// Figure out our first generation; it is the earliest generation in which an Eidos event is set up to run,
	// since an Eidos event that adds a subpopulation is necessary to get things started
	for (auto script_block : script_blocks)
		if (((script_block->type_ == SLiMEidosBlockType::SLiMEidosEventEarly) || (script_block->type_ == SLiMEidosBlockType::SLiMEidosEventLate))
			&& (script_block->start_generation_ < first_gen) && (script_block->start_generation_ > 0))
			first_gen = script_block->start_generation_;
	
	return first_gen;
}

slim_generation_t SLiMSim::EstimatedLastGeneration(void)
{
	// return our cached value if we have one
	if (last_script_block_gen_cached)
		return last_script_block_gen_;
	
	// otherwise, fill the cache
	std::vector<SLiMEidosBlock*> &script_blocks = AllScriptBlocks();
	slim_generation_t last_gen = 1;
	
	// The estimate is derived from the last generation in which an Eidos block is registered.
	// Any block type works, since the simulation could plausibly be stopped within a callback.
	// However, blocks that do not specify an end generation don't count.
	for (auto script_block : script_blocks)
		if ((script_block->end_generation_ > last_gen) && (script_block->end_generation_ != SLIM_MAX_GENERATION))
			last_gen = script_block->end_generation_;
	
	last_script_block_gen_ = last_gen;
	last_script_block_gen_cached = true;
	
	return last_script_block_gen_;
}

// This function is called only by the SLiM self-testing machinery.  It has no exception handling; raises will
// blow through to the catch block in the test harness so that they can be handled there.
bool SLiMSim::_RunOneGeneration(void)
{
	// ******************************************************************
	//
	// Stage 0: Pre-generation bookkeeping
	//
	generation_stage_ = SLiMGenerationStage::kStage0PreGeneration;
	
	// Define the current script around each generation execution, for error reporting
	gEidosCurrentScript = script_;
	gEidosExecutingRuntimeScript = false;
	
	// Activate all registered script blocks at the beginning of the generation
	std::vector<SLiMEidosBlock*> &script_blocks = AllScriptBlocks();
	
	for (SLiMEidosBlock *script_block : script_blocks)
		script_block->active_ = -1;
	
	if (generation_ == 0)
	{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
		// PROFILING
		SLIM_PROFILE_BLOCK_START();
#endif
		
		RunInitializeCallbacks();
		
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
		// PROFILING
		SLIM_PROFILE_BLOCK_END(profile_stage_totals_[0]);
#endif
		
		// Zero out error-reporting info so raises elsewhere don't get attributed to this script
		gEidosCurrentScript = nullptr;
		gEidosExecutingRuntimeScript = false;
		return true;
	}
	else
	{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
		// PROFILING
#if SLIM_USE_NONNEUTRAL_CACHES
		if (gEidosProfilingClientCount)
			CollectSLiMguiMutationProfileInfo();
#endif
#endif
		
		// make a clock if we're running experiments
		clock_t x_clock0 = (x_experiments_enabled_ ? clock() : 0);
		
		
		// ******************************************************************
		//
		// Stage 1: Execute early() script events for the current generation
		//
		{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_START();
#endif
			
			generation_stage_ = SLiMGenerationStage::kStage1ExecuteEarlyScripts;
			
			std::vector<SLiMEidosBlock*> early_blocks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosEventEarly, -1, -1, -1);
			
			for (auto script_block : early_blocks)
			{
				if (script_block->active_)
				{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
					// PROFILING
					SLIM_PROFILE_BLOCK_START_NESTED();
#endif
					
					population_.ExecuteScript(script_block, generation_, chromosome_);
					
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
					// PROFILING
					SLIM_PROFILE_BLOCK_END_NESTED(profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosEventEarly)]);
#endif
				}
			}
			
			// the stage is done, so deregister script blocks as requested
			DeregisterScheduledScriptBlocks();
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(profile_stage_totals_[1]);
#endif
		}
		
		
		// ******************************************************************
		//
		// Stage 2: Generate offspring: evolve all subpopulations
		//
		{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_START();
#endif
			
			CheckMutationStackPolicy();
			
			generation_stage_ = SLiMGenerationStage::kStage2GenerateOffspring;
			
			std::vector<SLiMEidosBlock*> mate_choice_callbacks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosMateChoiceCallback, -1, -1, -1);
			std::vector<SLiMEidosBlock*> modify_child_callbacks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosModifyChildCallback, -1, -1, -1);
			std::vector<SLiMEidosBlock*> recombination_callbacks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosRecombinationCallback, -1, -1, -1);
			bool mate_choice_callbacks_present = mate_choice_callbacks.size();
			bool modify_child_callbacks_present = modify_child_callbacks.size();
			bool recombination_callbacks_present = recombination_callbacks.size();
			bool no_active_callbacks = true;
			
			// if there are no active callbacks of any type, we can pretend there are no callbacks at all
			// if there is a callback of any type, however, then inactive callbacks could become active
			if (mate_choice_callbacks_present || modify_child_callbacks_present || recombination_callbacks_present)
			{
				for (SLiMEidosBlock *callback : mate_choice_callbacks)
					if (callback->active_)
					{
						no_active_callbacks = false;
						break;
					}
				
				if (no_active_callbacks)
					for (SLiMEidosBlock *callback : modify_child_callbacks)
						if (callback->active_)
						{
							no_active_callbacks = false;
							break;
						}
				
				if (no_active_callbacks)
					for (SLiMEidosBlock *callback : recombination_callbacks)
						if (callback->active_)
						{
							no_active_callbacks = false;
							break;
						}
			}
			
			if (no_active_callbacks)
			{
				for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
					population_.EvolveSubpopulation(*subpop_pair.second, chromosome_, generation_, false, false, false);
			}
			else
			{
				// cache a list of callbacks registered for each subpop
				for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
				{
					slim_objectid_t subpop_id = subpop_pair.first;
					Subpopulation *subpop = subpop_pair.second;
					
					// Get mateChoice() callbacks that apply to this subpopulation
					subpop->registered_mate_choice_callbacks_.clear();
					
					for (SLiMEidosBlock *callback : mate_choice_callbacks)
					{
						slim_objectid_t callback_subpop_id = callback->subpopulation_id_;
						
						if ((callback_subpop_id == -1) || (callback_subpop_id == subpop_id))
							subpop->registered_mate_choice_callbacks_.emplace_back(callback);
					}
					
					// Get modifyChild() callbacks that apply to this subpopulation
					subpop->registered_modify_child_callbacks_.clear();
					
					for (SLiMEidosBlock *callback : modify_child_callbacks)
					{
						slim_objectid_t callback_subpop_id = callback->subpopulation_id_;
						
						if ((callback_subpop_id == -1) || (callback_subpop_id == subpop_id))
							subpop->registered_modify_child_callbacks_.emplace_back(callback);
					}
					
					// Get recombination() callbacks that apply to this subpopulation
					subpop->registered_recombination_callbacks_.clear();
					
					for (SLiMEidosBlock *callback : recombination_callbacks)
					{
						slim_objectid_t callback_subpop_id = callback->subpopulation_id_;
						
						if ((callback_subpop_id == -1) || (callback_subpop_id == subpop_id))
							subpop->registered_recombination_callbacks_.emplace_back(callback);
					}
				}
				
				// then evolve each subpop
				for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
					population_.EvolveSubpopulation(*subpop_pair.second, chromosome_, generation_, mate_choice_callbacks_present, modify_child_callbacks_present, recombination_callbacks_present);
			}
			
			// then switch to the child generation; we don't want to do this until all callbacks have executed for all subpops
			for (std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
				subpop_pair.second->child_generation_valid_ = true;
			
			population_.child_generation_valid_ = true;
			
			// the stage is done, so deregister script blocks as requested
			DeregisterScheduledScriptBlocks();
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(profile_stage_totals_[2]);
#endif
		}
		
		
		// ******************************************************************
		//
		// Stage 3: Remove fixed mutations and associated tasks
		//
		{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_START();
#endif
			
			generation_stage_ = SLiMGenerationStage::kStage3RemoveFixedMutations;
			
			population_.ClearParentalGenomes();		// added 30 November 2016 so MutationRun refcounts reflect their usage count in the simulation
			
			population_.MaintainRegistry();
			
			// Every hundredth generation we unique mutation runs to optimize memory usage and efficiency.  The number 100 was
			// picked out of a hat – often enough to perhaps be useful in keeping SLiM slim, but infrequent enough that if it
			// is a time sink it won't impact the simulation too much.  This call is really quite fast, though – on the order
			// of 0.015 seconds for a pop of 10000 with a 1e5 chromosome and lots of mutations.  So although doing this every
			// generation would seem like overkill – very few duplicates would be found per call – every 100 should be fine.
			// Anyway, if we start seeing this call in performance analysis, we should probably revisit this; the benefit is
			// likely to be pretty small for most simulations, so if the cost is significant then it may be a lose.
			if (generation_ % 100 == 0)
				population_.UniqueMutationRuns();
			
			// Invalidate interactions, now that the generation they were valid for is disappearing
			for (auto int_type = interaction_types_.begin(); int_type != interaction_types_.end(); ++int_type)
				int_type->second->Invalidate();
			
			// Deregister any interaction() callbacks that have been scheduled for deregistration, since it is now safe to do so
			DeregisterScheduledInteractionBlocks();
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(profile_stage_totals_[3]);
#endif
		}
		
		
		// ******************************************************************
		//
		// Stage 4: Swap generations
		//
		{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_START();
#endif
			
			generation_stage_ = SLiMGenerationStage::kStage4SwapGenerations;
			
			population_.SwapGenerations();
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(profile_stage_totals_[4]);
#endif
		}
		
		
		// ******************************************************************
		//
		// Stage 5: Execute late() script events for the current generation
		//
		{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_START();
#endif
			
			generation_stage_ = SLiMGenerationStage::kStage5ExecuteLateScripts;
			
			std::vector<SLiMEidosBlock*> late_blocks = ScriptBlocksMatching(generation_, SLiMEidosBlockType::SLiMEidosEventLate, -1, -1, -1);
			
			for (auto script_block : late_blocks)
			{
				if (script_block->active_)
				{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
					// PROFILING
					SLIM_PROFILE_BLOCK_START_NESTED();
#endif
					
					population_.ExecuteScript(script_block, generation_, chromosome_);
					
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
					// PROFILING
					SLIM_PROFILE_BLOCK_END_NESTED(profile_callback_totals_[(int)(SLiMEidosBlockType::SLiMEidosEventLate)]);
#endif
				}
			}
			
			// the stage is done, so deregister script blocks as requested
			DeregisterScheduledScriptBlocks();
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(profile_stage_totals_[5]);
#endif
		}
		
		
		// ******************************************************************
		//
		// Stage 6: Calculate fitness values for the new parental generation
		//
		{
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_START();
#endif
			
			generation_stage_ = SLiMGenerationStage::kStage6CalculateFitness;
			
			population_.RecalculateFitness(generation_);	// used to be generation_ + 1; removing that 18 Feb 2016 BCH
			
			// the stage is done, so deregister script blocks as requested
			DeregisterScheduledScriptBlocks();
			
			// Maintain our mutation run experiments; we want this overhead to appear within the stage 6 profile
			if (x_experiments_enabled_)
				MaintainMutationRunExperiments((clock() - x_clock0) / (double)CLOCKS_PER_SEC);
			
#if defined(SLIMGUI) && (SLIMPROFILING == 1)
			// PROFILING
			SLIM_PROFILE_BLOCK_END(profile_stage_totals_[6]);
#endif
			
#ifdef SLIMGUI
			// Let SLiMgui survey the population for mean fitness and such, if it is our target
			// We do this outside of profiling and mutation run experiments, since SLiMgui overhead should not affect those
			population_.SurveyPopulation();
#endif
		}
		
		
		// ******************************************************************
		//
		// Stage 7: Advance the generation counter and do end-generation tasks
		//
		{
			generation_stage_ = SLiMGenerationStage::kStage7AdvanceGenerationCounter;
			
#ifdef SLIMGUI
			// re-tally for SLiMgui; this should avoid doing any new work if no mutations have been added or removed since the last tally
			// it is needed, though, so that if the user added/removed mutations in a late() event SLiMgui displays correctly
			// NOTE that this means tallies may be different in SLiMgui than in slim!  I *think* this will never be visible to the
			// user's model, because if they ask for mutation counts/frequences a call to TallyMutationReferences() will be made at that
			// point anyway to synchronize; but in slim's code itself, not in Eidos, the tallies can definitely differ!  Beware!
			population_.TallyMutationReferences(nullptr, false);
#endif
			
			cached_value_generation_.reset();
			generation_++;
			
			// Zero out error-reporting info so raises elsewhere don't get attributed to this script
			gEidosCurrentScript = nullptr;
			gEidosExecutingRuntimeScript = false;
			
			// Decide whether the simulation is over.  We need to call EstimatedLastGeneration() every time; we can't
			// cache it, because it can change based upon changes in script registration / deregistration.
			bool result;
			
			if (sim_declared_finished_)
				result = false;
			else
				result = (generation_ <= EstimatedLastGeneration());
			
			if (!result)
				SimulationFinished();
			
			return result;
		}
	}
}

// This function is called by both SLiM and SLiMgui to run a generation.  In SLiM, it simply calls _RunOneGeneration(),
// with no exception handling; in that scenario exceptions should not be thrown, since eidos_terminate() will log an
// error and then call exit().  In SLiMgui, eidos_terminate() will raise an exception, and it will be caught right
// here and converted to an "invalid simulation" state (simulationValid == false), which will be noticed by SLiMgui
// and will cause error reporting to occur based upon the error-tracking variables set.
bool SLiMSim::RunOneGeneration(void)
{
#ifdef SLIMGUI
	if (simulationValid)
	{
		try
		{
#endif
			return _RunOneGeneration();
#ifdef SLIMGUI
		}
		catch (...)
		{
			simulationValid = false;
			
			// In the event of a raise, we clear gEidosCurrentScript, which is not normally part of the error-
			// reporting state, but is used only to inform eidos_terminate() about the current script at the point
			// when a raise occurs.  We don't want raises after RunOneGeneration() returns to be attributed to us,
			// so we clear the script pointer.  We do NOT clear any of the error-reporting state, since it will
			// be used by higher levels to select the error in the GUI.
			gEidosCurrentScript = nullptr;
			return false;
		}
	}
	
	gEidosCurrentScript = nullptr;
#endif
	
	return false;
}

void SLiMSim::SimulationFinished(void)
{
	// This is an opportunity for final calculation/output when a simulation finishes
	
#if MUTRUN_EXPERIMENT_OUTPUT
	// Print a full mutation run count history if MUTRUN_EXPERIMENT_OUTPUT is enabled
	if (SLiM_verbose_output && x_experiments_enabled_)
	{
		SLIM_OUTSTREAM << std::endl;
		SLIM_OUTSTREAM << "// Mutrun count history:" << std::endl;
		SLIM_OUTSTREAM << "// mutrun_history <- c(";
		
		bool first_count = true;
		
		for (int32_t count : x_mutcount_history_)
		{
			if (first_count)
				first_count = false;
			else
				SLIM_OUTSTREAM << ", ";
			
			SLIM_OUTSTREAM << count;
		}
		
		SLIM_OUTSTREAM << ")" << std::endl << std::endl;
	}
#endif
	
	// If verbose output is enabled and we've been running mutation run experiments,
	// figure out the modal mutation run count and print that, for the user's benefit.
	if (SLiM_verbose_output && x_experiments_enabled_)
	{
		int modal_index, modal_tally;
		int power_tallies[20];	// we only go up to 1024 mutruns right now, but this gives us some headroom
		
		for (int i = 0; i < 20; ++i)
			power_tallies[i] = 0;
		
		for (int32_t count : x_mutcount_history_)
		{
			int32_t power = (int32_t)round(log2(count));
			
			power_tallies[power]++;
		}
		
		modal_index = -1;
		modal_tally = -1;
		
		for (int i = 0; i < 20; ++i)
			if (power_tallies[i] > modal_tally)
			{
				modal_tally = power_tallies[i];
				modal_index = i;
			}
		
		int modal_count = (int)round(pow(2.0, modal_index));
		double modal_fraction = power_tallies[modal_index] / (double)(x_mutcount_history_.size());
		
		SLIM_OUTSTREAM << std::endl;
		SLIM_OUTSTREAM << "// Mutation run modal count: " << modal_count << " (" << (modal_fraction * 100) << "% of generations)" << std::endl;
		SLIM_OUTSTREAM << "//" << std::endl;
		SLIM_OUTSTREAM << "// It might (or might not) speed up your model to add a call to:" << std::endl;
		SLIM_OUTSTREAM << "//" << std::endl;
		SLIM_OUTSTREAM << "//    initializeSLiMOptions(mutationRuns=" << modal_count << ");" << std::endl;
		SLIM_OUTSTREAM << "//" << std::endl;
		SLIM_OUTSTREAM << "// to your initialize() callback.  The optimal value will change" << std::endl;
		SLIM_OUTSTREAM << "// if your model changes.  See the SLiM manual for more details." << std::endl;
		SLIM_OUTSTREAM << std::endl;
	}
}

void SLiMSim::_CheckMutationStackPolicy(void)
{
	// Check mutation stacking policy for consistency.  This is called periodically during the simulation.
	std::vector<int64_t> checked_groups;
	
	for (auto muttype_iter : mutation_types_)
	{
		MutationType *muttype = muttype_iter.second;
		int64_t stack_group = muttype->stack_group_;
		
		if (std::find(checked_groups.begin(), checked_groups.end(), stack_group) == checked_groups.end())
		{
			// This stacking group has not been checked yet
			MutationStackPolicy stack_policy = muttype->stack_policy_;
			
			for (auto muttype_iter2 : mutation_types_)
			{
				MutationType *muttype2 = muttype_iter2.second;
				
				if ((muttype2->stack_group_ == stack_group) && (muttype2->stack_policy_ != stack_policy))
					EIDOS_TERMINATION << "ERROR (SLiMSim::_CheckMutationStackPolicy): inconsistent mutationStackPolicy values within one mutationStackGroup." << eidos_terminate();
			}
			
			checked_groups.push_back(stack_group);
		}
	}
	
	// we're good until the next change
	mutation_stack_policy_changed = false;
}


//
//	Eidos support
//
#pragma mark -
#pragma mark Eidos support

EidosValue_SP SLiMSim::ContextDefinedFunctionDispatch(const std::string &p_function_name, const EidosValue_SP *const p_arguments, int p_argument_count, EidosInterpreter &p_interpreter)
{
#pragma unused(p_interpreter)
	
	EidosValue *arg0_value = ((p_argument_count >= 1) ? p_arguments[0].get() : nullptr);
	EidosValue *arg1_value = ((p_argument_count >= 2) ? p_arguments[1].get() : nullptr);
	EidosValue *arg2_value = ((p_argument_count >= 3) ? p_arguments[2].get() : nullptr);
	EidosValue *arg3_value = ((p_argument_count >= 4) ? p_arguments[3].get() : nullptr);
	EidosValue *arg4_value = ((p_argument_count >= 5) ? p_arguments[4].get() : nullptr);
	std::ostringstream &output_stream = p_interpreter.ExecutionOutputStream();
	
	// we only define initialize...() functions; so we must be in an initialize() callback
	if (generation_ != 0)
		EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): the function " << p_function_name << "() may only be called in an initialize() callback." << eidos_terminate();
	
	
	//
	//	*********************	(void)initializeGenomicElement(io<GenomicElementType>$ genomicElementType, integer$ start, integer$ end)
	//
	#pragma mark initializeGenomicElement()
	
	if (p_function_name.compare(gStr_initializeGenomicElement) == 0)
	{
		GenomicElementType *genomic_element_type_ptr = nullptr;
		slim_position_t start_position = SLiMCastToPositionTypeOrRaise(arg1_value->IntAtIndex(0, nullptr));
		slim_position_t end_position = SLiMCastToPositionTypeOrRaise(arg2_value->IntAtIndex(0, nullptr));
		
		if (arg0_value->Type() == EidosValueType::kValueInt)
		{
			slim_objectid_t genomic_element_type = SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr));
			auto found_getype_pair = genomic_element_types_.find(genomic_element_type);
			
			if (found_getype_pair == genomic_element_types_.end())
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElement() genomic element type g" << genomic_element_type << " not defined." << eidos_terminate();
			
			genomic_element_type_ptr = found_getype_pair->second;
		}
		else
		{
			genomic_element_type_ptr = dynamic_cast<GenomicElementType *>(arg0_value->ObjectElementAtIndex(0, nullptr));
		}
		
		if (end_position < start_position)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElement() end position " << end_position << " is less than start position " << start_position << "." << eidos_terminate();
		
		// Check that the new element will not overlap any existing element; if end_position > last_genomic_element_position we are safe.
		// Otherwise, we have to check all previously defined elements.  The use of last_genomic_element_position is an optimization to
		// avoid an O(N) scan with each added element; as long as elements are added in sorted order there is no need to scan.
		if (start_position <= last_genomic_element_position)
		{
			for (auto &element : chromosome_)
			{
				if ((element.start_position_ <= end_position) && (element.end_position_ >= start_position))
					EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElement() genomic element from start position " << start_position << " to end position " << end_position << " overlaps existing genomic element." << eidos_terminate();
			}
		}
		
		if (end_position > last_genomic_element_position)
			last_genomic_element_position = end_position;
		
		// Create and add the new element
		GenomicElement new_genomic_element(genomic_element_type_ptr, start_position, end_position);
		
		bool old_log = GenomicElement::LogGenomicElementCopyAndAssign(false);
		chromosome_.emplace_back(new_genomic_element);
		GenomicElement::LogGenomicElementCopyAndAssign(old_log);
		
		chromosome_changed_ = true;
		
		if (DEBUG_INPUT)
		{
			if (ABBREVIATE_DEBUG_INPUT && (num_genomic_elements_ > 99))
			{
				if (num_genomic_elements_ == 100)
					output_stream << "(...more initializeGenomicElement() calls omitted...)" << endl;
			}
			else
			{
				output_stream << "initializeGenomicElement(g" << genomic_element_type_ptr->genomic_element_type_id_ << ", " << start_position << ", " << end_position << ");" << endl;
			}
		}
		
		num_genomic_elements_++;
	}
	
	
	//
	//	*********************	(object<GenomicElementType>$)initializeGenomicElementType(is$ id, io<MutationType> mutationTypes, numeric proportions)
	//
	#pragma mark initializeGenomicElementType()
	
	else if (p_function_name.compare(gStr_initializeGenomicElementType) == 0)
	{
		slim_objectid_t map_identifier = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 'g', nullptr);
		
		if (genomic_element_types_.count(map_identifier) > 0) 
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElementType() genomic element type g" << map_identifier << " already defined." << eidos_terminate();
		
		int mut_type_id_count = arg1_value->Count();
		int proportion_count = arg2_value->Count();
		
		if (mut_type_id_count != proportion_count)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElementType() requires the sizes of mutationTypes and proportions to be equal." << eidos_terminate();
		
		std::vector<MutationType*> mutation_types;
		std::vector<double> mutation_fractions;
		
		for (int mut_type_index = 0; mut_type_index < mut_type_id_count; ++mut_type_index)
		{
			MutationType *mutation_type_ptr = nullptr;
			double proportion = arg2_value->FloatAtIndex(mut_type_index, nullptr);
			
			if (proportion < 0)		// == 0 is allowed but must be fixed before the simulation executes; see InitializeDraws()
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElementType() proportions must be greater than or equal to zero (" << proportion << " supplied)." << eidos_terminate();
			
			if (arg1_value->Type() == EidosValueType::kValueInt)
			{
				slim_objectid_t mutation_type_id = SLiMCastToObjectidTypeOrRaise(arg1_value->IntAtIndex(mut_type_index, nullptr));
				auto found_muttype_pair = mutation_types_.find(mutation_type_id);
				
				if (found_muttype_pair != mutation_types_.end())
					mutation_type_ptr = found_muttype_pair->second;
				
				if (!mutation_type_ptr)
					EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElementType() mutation type m" << mutation_type_id << " not defined." << eidos_terminate();
			}
			else
			{
				mutation_type_ptr = dynamic_cast<MutationType *>(arg1_value->ObjectElementAtIndex(mut_type_index, nullptr));
			}
			
			if (std::find(mutation_types.begin(), mutation_types.end(), mutation_type_ptr) != mutation_types.end())
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElementType() mutation type m" << mutation_type_ptr->mutation_type_id_ << " used more than once." << eidos_terminate();
			
			mutation_types.emplace_back(mutation_type_ptr);
			mutation_fractions.emplace_back(proportion);
			
			// check whether we are using a mutation type that is non-neutral; check and set pure_neutral_
			if ((mutation_type_ptr->dfe_type_ != DFEType::kFixed) || (mutation_type_ptr->dfe_parameters_[0] != 0.0))
			{
				SLiMSim *sim = dynamic_cast<SLiMSim *>(p_interpreter.Context());
				
				if (sim)
					sim->pure_neutral_ = false;
			}
		}
		
		GenomicElementType *new_genomic_element_type = new GenomicElementType(map_identifier, mutation_types, mutation_fractions);
		genomic_element_types_.insert(std::pair<const slim_objectid_t,GenomicElementType*>(map_identifier, new_genomic_element_type));
		genomic_element_types_changed_ = true;
		
		// define a new Eidos variable to refer to the new genomic element type
		EidosSymbolTableEntry &symbol_entry = new_genomic_element_type->SymbolTableEntry();
		
		if (p_interpreter.SymbolTable().ContainsSymbol(symbol_entry.first))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGenomicElementType() symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate();
		
		simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
		
		if (DEBUG_INPUT)
		{
			if (ABBREVIATE_DEBUG_INPUT && (num_genomic_element_types_ > 99))
			{
				if (num_genomic_element_types_ == 100)
					output_stream << "(...more initializeGenomicElementType() calls omitted...)" << endl;
			}
			else
			{
				output_stream << "initializeGenomicElementType(" << map_identifier;
				
				output_stream << ((mut_type_id_count > 1) ? ", c(" : ", ");
				for (int mut_type_index = 0; mut_type_index < mut_type_id_count; ++mut_type_index)
					output_stream << (mut_type_index > 0 ? ", m" : "m") << mutation_types[mut_type_index]->mutation_type_id_;
				output_stream << ((mut_type_id_count > 1) ? ")" : "");
				
				output_stream << ((mut_type_id_count > 1) ? ", c(" : ", ");
				for (int mut_type_index = 0; mut_type_index < mut_type_id_count; ++mut_type_index)
					output_stream << (mut_type_index > 0 ? ", " : "") << arg2_value->FloatAtIndex(mut_type_index, nullptr);
				output_stream << ((mut_type_id_count > 1) ? ")" : "");
				
				output_stream << ");" << endl;
			}
		}
		
		num_genomic_element_types_++;
		return symbol_entry.second;
	}
	
	
	//
	//	*********************	(object<InteractionType>$)initializeInteractionType(is$ id, string$ spatiality, [logical$ reciprocal = F], [numeric$ maxDistance = INF], [string$ sexSegregation = "**"])
	//
	#pragma mark initializeInteractionType()
	
	else if (p_function_name.compare(gStr_initializeInteractionType) == 0)
	{
		slim_objectid_t map_identifier = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 'i', nullptr);
		string spatiality_string = arg1_value->StringAtIndex(0, nullptr);
		bool reciprocal = arg2_value->LogicalAtIndex(0, nullptr);
		double max_distance = arg3_value->FloatAtIndex(0, nullptr);
		string sex_string = arg4_value->StringAtIndex(0, nullptr);
		int required_dimensionality;
		IndividualSex receiver_sex = IndividualSex::kUnspecified, exerter_sex = IndividualSex::kUnspecified;
		
		if (interaction_types_.count(map_identifier) > 0) 
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() mutation type m" << map_identifier << " already defined." << eidos_terminate();
		
		if (spatiality_string.length() == 0)					required_dimensionality = 0;
		else if (spatiality_string.compare(gEidosStr_x) == 0)	required_dimensionality = 1;
		else if (spatiality_string.compare(gEidosStr_y) == 0)	required_dimensionality = 2;
		else if (spatiality_string.compare(gEidosStr_z) == 0)	required_dimensionality = 3;
		else if (spatiality_string.compare("xy") == 0)			required_dimensionality = 2;
		else if (spatiality_string.compare("xz") == 0)			required_dimensionality = 3;
		else if (spatiality_string.compare("yz") == 0)			required_dimensionality = 3;
		else if (spatiality_string.compare("xyz") == 0)			required_dimensionality = 3;
		else
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() spatiality \"" << spatiality_string << "\" must be \"\", \"x\", \"y\", \"z\", \"xy\", \"xz\", \"yz\", or \"xyz\"." << eidos_terminate();
		
		if (required_dimensionality > spatial_dimensionality_)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() spatiality cannot utilize spatial dimensions beyond those set in initializeSLiMOptions()." << eidos_terminate();
		
		if (max_distance < 0.0)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() maxDistance must be >= 0.0." << eidos_terminate();
		if ((required_dimensionality == 0) && (!std::isinf(max_distance) || (max_distance < 0.0)))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() maxDistance must be INF for non-spatial interactions." << eidos_terminate();
		
		if (sex_string == "**")			{ receiver_sex = IndividualSex::kUnspecified;	exerter_sex = IndividualSex::kUnspecified;	}
		else if (sex_string == "*M")	{ receiver_sex = IndividualSex::kUnspecified;	exerter_sex = IndividualSex::kMale;			}
		else if (sex_string == "*F")	{ receiver_sex = IndividualSex::kUnspecified;	exerter_sex = IndividualSex::kFemale;		}
		else if (sex_string == "M*")	{ receiver_sex = IndividualSex::kMale;			exerter_sex = IndividualSex::kUnspecified;	}
		else if (sex_string == "MM")	{ receiver_sex = IndividualSex::kMale;			exerter_sex = IndividualSex::kMale;			}
		else if (sex_string == "MF")	{ receiver_sex = IndividualSex::kMale;			exerter_sex = IndividualSex::kFemale;		}
		else if (sex_string == "F*")	{ receiver_sex = IndividualSex::kFemale;		exerter_sex = IndividualSex::kUnspecified;	}
		else if (sex_string == "FM")	{ receiver_sex = IndividualSex::kFemale;		exerter_sex = IndividualSex::kMale;			}
		else if (sex_string == "FF")	{ receiver_sex = IndividualSex::kFemale;		exerter_sex = IndividualSex::kFemale;		}
		else
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() unsupported sexSegregation value (must be '**', '*M', '*F', 'M*', 'MM', 'MF', 'F*', 'FM', or 'FF')." << eidos_terminate();
		
		if (((receiver_sex != IndividualSex::kUnspecified) || (exerter_sex != IndividualSex::kUnspecified)) && !sex_enabled_)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() sexSegregation value other than '**' unsupported in non-sexual simulation." << eidos_terminate();
		
		InteractionType *new_interaction_type = new InteractionType(map_identifier, spatiality_string, reciprocal, max_distance, receiver_sex, exerter_sex);
		
		interaction_types_.insert(std::pair<const slim_objectid_t,InteractionType*>(map_identifier, new_interaction_type));
		interaction_types_changed_ = true;
		
		// define a new Eidos variable to refer to the new mutation type
		EidosSymbolTableEntry &symbol_entry = new_interaction_type->SymbolTableEntry();
		
		if (p_interpreter.SymbolTable().ContainsSymbol(symbol_entry.first))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeInteractionType() symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate();
		
		simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
		
		if (DEBUG_INPUT)
		{
			output_stream << "initializeInteractionType(" << map_identifier << ", \"" << spatiality_string;
			
			if (reciprocal == true)
				output_stream << "\", reciprocal=T";
			
			if (!std::isinf(max_distance))
				output_stream << "\", maxDistance=" << max_distance;
			
			if (sex_string != "**")
				output_stream << "\", sexSegregation=" << sex_string;
			
			output_stream << ");" << endl;
		}
		
		num_interaction_types_++;
		return symbol_entry.second;
	}
	
	
	//
	//	*********************	(object<MutationType>$)initializeMutationType(is$ id, numeric$ dominanceCoeff, string$ distributionType, ...)
	//
	#pragma mark initializeMutationType()
	
	else if (p_function_name.compare(gStr_initializeMutationType) == 0)
	{
		slim_objectid_t map_identifier = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 'm', nullptr);
		double dominance_coeff = arg1_value->FloatAtIndex(0, nullptr);
		string dfe_type_string = arg2_value->StringAtIndex(0, nullptr);
		DFEType dfe_type;
		int expected_dfe_param_count = 0;
		std::vector<double> dfe_parameters;
		std::vector<std::string> dfe_strings;
		bool numericParams = true;		// if true, params must be int/float; if false, params must be string
		
		if (mutation_types_.count(map_identifier) > 0) 
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationType() mutation type m" << map_identifier << " already defined." << eidos_terminate();
		
		if (dfe_type_string.compare(gStr_f) == 0)
		{
			dfe_type = DFEType::kFixed;
			expected_dfe_param_count = 1;
		}
		else if (dfe_type_string.compare(gStr_g) == 0)
		{
			dfe_type = DFEType::kGamma;
			expected_dfe_param_count = 2;
		}
		else if (dfe_type_string.compare(gStr_e) == 0)
		{
			dfe_type = DFEType::kExponential;
			expected_dfe_param_count = 1;
		}
		else if (dfe_type_string.compare(gEidosStr_n) == 0)
		{
			dfe_type = DFEType::kNormal;
			expected_dfe_param_count = 2;
		}
		else if (dfe_type_string.compare(gStr_w) == 0)
		{
			dfe_type = DFEType::kWeibull;
			expected_dfe_param_count = 2;
		}
		else if (dfe_type_string.compare(gStr_s) == 0)
		{
			dfe_type = DFEType::kScript;
			expected_dfe_param_count = 1;
			numericParams = false;
		}
		else
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationType() distributionType \"" << dfe_type_string << "\" must be \"f\", \"g\", \"e\", \"n\", \"w\", or \"s\"." << eidos_terminate();
		
		if (p_argument_count != 3 + expected_dfe_param_count)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationType() distributionType \"" << dfe_type << "\" requires exactly " << expected_dfe_param_count << " DFE parameter" << (expected_dfe_param_count == 1 ? "" : "s") << "." << eidos_terminate();
		
		for (int dfe_param_index = 0; dfe_param_index < expected_dfe_param_count; ++dfe_param_index)
		{
			EidosValue *dfe_param_value = p_arguments[3 + dfe_param_index].get();
			EidosValueType dfe_param_type = dfe_param_value->Type();
			
			if (numericParams)
			{
				if ((dfe_param_type != EidosValueType::kValueFloat) && (dfe_param_type != EidosValueType::kValueInt))
					EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationType() requires that DFE parameters be numeric (integer or float)." << eidos_terminate();
				
				dfe_parameters.emplace_back(dfe_param_value->FloatAtIndex(0, nullptr));
				// intentionally no bounds checks for DFE parameters
			}
			else
			{
				if (dfe_param_type != EidosValueType::kValueString)
					EIDOS_TERMINATION << "ERROR (MutationType::ExecuteInstanceMethod): initializeMutationType() requires that the parameters for this DFE be of type string." << eidos_terminate();
				
				dfe_strings.emplace_back(dfe_param_value->StringAtIndex(0, nullptr));
				// intentionally no bounds checks for DFE parameters
			}
		}
		
#ifdef SLIMGUI
		// each new mutation type gets a unique zero-based index, used by SLiMgui to categorize mutations
		MutationType *new_mutation_type = new MutationType(*this, map_identifier, dominance_coeff, dfe_type, dfe_parameters, dfe_strings, num_mutation_types_);
#else
		MutationType *new_mutation_type = new MutationType(*this, map_identifier, dominance_coeff, dfe_type, dfe_parameters, dfe_strings);
#endif
		
		mutation_types_.insert(std::pair<const slim_objectid_t,MutationType*>(map_identifier, new_mutation_type));
		mutation_types_changed_ = true;
		
		// define a new Eidos variable to refer to the new mutation type
		EidosSymbolTableEntry &symbol_entry = new_mutation_type->SymbolTableEntry();
		
		if (p_interpreter.SymbolTable().ContainsSymbol(symbol_entry.first))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationType() symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate();
		
		simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
		
		if (DEBUG_INPUT)
		{
			if (ABBREVIATE_DEBUG_INPUT && (num_mutation_types_ > 99))
			{
				if (num_mutation_types_ == 100)
					output_stream << "(...more initializeMutationType() calls omitted...)" << endl;
			}
			else
			{
				output_stream << "initializeMutationType(" << map_identifier << ", " << dominance_coeff << ", \"" << dfe_type << "\"";
				
				if (numericParams)
				{
					for (double dfe_param : dfe_parameters)
						output_stream << ", " << dfe_param;
				}
				else
				{
					for (std::string dfe_param : dfe_strings)
						output_stream << ", \"" << dfe_param << "\"";
				}
				
				output_stream << ");" << endl;
			}
		}
		
		num_mutation_types_++;
		return symbol_entry.second;
	}
	
	
	//
	//	*********************	(void)initializeRecombinationRate(numeric rates, [Ni ends = NULL], [string$ sex = "*"])
	//
	#pragma mark initializeRecombinationRate()
	
	else if (p_function_name.compare(gStr_initializeRecombinationRate) == 0)
	{
		int rate_count = arg0_value->Count();
		
		// Figure out what sex we are being given a map for
		IndividualSex requested_sex;
		std::string sex_string = arg2_value->StringAtIndex(0, nullptr);
		
		if (sex_string.compare("M") == 0)
			requested_sex = IndividualSex::kMale;
		else if (sex_string.compare("F") == 0)
			requested_sex = IndividualSex::kFemale;
		else if (sex_string.compare("*") == 0)
			requested_sex = IndividualSex::kUnspecified;
		else
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() requested sex \"" << sex_string << "\" unsupported." << eidos_terminate();
		
		if ((requested_sex != IndividualSex::kUnspecified) && !sex_enabled_)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() sex-specific recombination map supplied in non-sexual simulation." << eidos_terminate();
		
		// Make sure specifying a map for that sex is legal, given our current state.  Since single_recombination_map_ has not been set
		// yet, we just look to see whether the chromosome's policy has already been determined or not.
		if (((requested_sex == IndividualSex::kUnspecified) && ((chromosome_.recombination_rates_M_.size() != 0) || (chromosome_.recombination_rates_F_.size() != 0))) ||
			((requested_sex != IndividualSex::kUnspecified) && (chromosome_.recombination_rates_H_.size() != 0)))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() cannot change the chromosome between using a single map versus separate maps for the sexes; the original configuration must be preserved." << eidos_terminate();
		
		if (((requested_sex == IndividualSex::kUnspecified) && (num_recombination_rates_ > 0)) || ((requested_sex != IndividualSex::kUnspecified) && (num_recombination_rates_ > 1)))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() may be called only once (or once per sex, with sex-specific recombination maps).  The multiple recombination regions of a recombination map must be set up in a single call to initializeRecombinationRate()." << eidos_terminate();
		
		// Set up to replace the requested map
		vector<slim_position_t> &positions = ((requested_sex == IndividualSex::kUnspecified) ? chromosome_.recombination_end_positions_H_ : 
											  ((requested_sex == IndividualSex::kMale) ? chromosome_.recombination_end_positions_M_ : chromosome_.recombination_end_positions_F_));
		vector<double> &rates = ((requested_sex == IndividualSex::kUnspecified) ? chromosome_.recombination_rates_H_ : 
								 ((requested_sex == IndividualSex::kMale) ? chromosome_.recombination_rates_M_ : chromosome_.recombination_rates_F_));
		
		if (arg1_value->Type() == EidosValueType::kValueNULL)
		{
			if (rate_count != 1)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() requires rates to be a singleton if ends is not supplied." << eidos_terminate();
			
			double recombination_rate = arg0_value->FloatAtIndex(0, nullptr);
			
			// check values
			if (recombination_rate < 0.0)		// intentionally no upper bound
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() requires rates to be >= 0 (" << recombination_rate << " supplied)." << eidos_terminate();
			
			// then adopt them
			rates.clear();
			positions.clear();
			
			rates.emplace_back(recombination_rate);
			//positions.emplace_back(?);	// deferred; patched in Chromosome::InitializeDraws().
		}
		else
		{
			int end_count = arg1_value->Count();
			
			if ((end_count != rate_count) || (end_count == 0))
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() requires ends and rates to be of equal and nonzero size." << eidos_terminate();
			
			// check values
			for (int value_index = 0; value_index < end_count; ++value_index)
			{
				double recombination_rate = arg0_value->FloatAtIndex(value_index, nullptr);
				slim_position_t recombination_end_position = SLiMCastToPositionTypeOrRaise(arg1_value->IntAtIndex(value_index, nullptr));
				
				if (value_index > 0)
					if (recombination_end_position <= arg1_value->IntAtIndex(value_index - 1, nullptr))
						EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() requires ends to be in strictly ascending order." << eidos_terminate();
				
				if (recombination_rate < 0.0)		// intentionally no upper bound
					EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeRecombinationRate() requires rates to be >= 0 (" << recombination_rate << " supplied)." << eidos_terminate();
			}
			
			// then adopt them
			rates.clear();
			positions.clear();
			
			for (int interval_index = 0; interval_index < end_count; ++interval_index)
			{
				double recombination_rate = arg0_value->FloatAtIndex(interval_index, nullptr);
				slim_position_t recombination_end_position = SLiMCastToPositionTypeOrRaise(arg1_value->IntAtIndex(interval_index, nullptr));
				
				rates.emplace_back(recombination_rate);
				positions.emplace_back(recombination_end_position);
			}
		}
		
		chromosome_changed_ = true;
		
		if (DEBUG_INPUT)
		{
			int ratesSize = (int)rates.size();
			int endsSize = (int)positions.size();
			
			output_stream << "initializeRecombinationRate(";
			
			if (ratesSize > 1)
				output_stream << "c(";
			for (int interval_index = 0; interval_index < ratesSize; ++interval_index)
				output_stream << (interval_index == 0 ? "" : ", ") << rates[interval_index];
			if (ratesSize > 1)
				output_stream << ")";
			
			if (endsSize > 0)
			{
				output_stream << ", ";
				
				if (endsSize > 1)
					output_stream << "c(";
				for (int interval_index = 0; interval_index < endsSize; ++interval_index)
					output_stream << (interval_index == 0 ? "" : ", ") << positions[interval_index];
				if (endsSize > 1)
					output_stream << ")";
			}
			
			output_stream << ");" << endl;
		}
		
		num_recombination_rates_++;
	}
	
	
	//
	//	*********************	(void)initializeGeneConversion(numeric$ conversionFraction, numeric$ meanLength)
	//
	#pragma mark initializeGeneConversion()
	
	else if (p_function_name.compare(gStr_initializeGeneConversion) == 0)
	{
		if (num_gene_conversions_ > 0)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGeneConversion() may be called only once." << eidos_terminate();
		
		double gene_conversion_fraction = arg0_value->FloatAtIndex(0, nullptr);
		double gene_conversion_avg_length = arg1_value->FloatAtIndex(0, nullptr);
		
		if ((gene_conversion_fraction < 0.0) || (gene_conversion_fraction > 1.0))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGeneConversion() conversionFraction must be between 0.0 and 1.0 inclusive (" << gene_conversion_fraction << " supplied)." << eidos_terminate();
		if (gene_conversion_avg_length <= 0.0)		// intentionally no upper bound
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeGeneConversion() meanLength must be greater than 0.0 (" << gene_conversion_avg_length << " supplied)." << eidos_terminate();
		
		chromosome_.gene_conversion_fraction_ = gene_conversion_fraction;
		chromosome_.gene_conversion_avg_length_ = gene_conversion_avg_length;
		
		if (DEBUG_INPUT)
			output_stream << "initializeGeneConversion(" << gene_conversion_fraction << ", " << gene_conversion_avg_length << ");" << endl;
		
		num_gene_conversions_++;
	}
	
	
	//
	//	*********************	(void)initializeMutationRate(numeric rates, [Ni ends = NULL], [string$ sex = "*"])
	//
	#pragma mark initializeMutationRate()
	
	else if (p_function_name.compare(gStr_initializeMutationRate) == 0)
	{
		int rate_count = arg0_value->Count();
		
		// Figure out what sex we are being given a map for
		IndividualSex requested_sex;
		std::string sex_string = arg2_value->StringAtIndex(0, nullptr);
		
		if (sex_string.compare("M") == 0)
			requested_sex = IndividualSex::kMale;
		else if (sex_string.compare("F") == 0)
			requested_sex = IndividualSex::kFemale;
		else if (sex_string.compare("*") == 0)
			requested_sex = IndividualSex::kUnspecified;
		else
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() requested sex \"" << sex_string << "\" unsupported." << eidos_terminate();
		
		if ((requested_sex != IndividualSex::kUnspecified) && !sex_enabled_)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() sex-specific mutation map supplied in non-sexual simulation." << eidos_terminate();
		
		// Make sure specifying a map for that sex is legal, given our current state.  Since single_mutation_map_ has not been set
		// yet, we just look to see whether the chromosome's policy has already been determined or not.
		if (((requested_sex == IndividualSex::kUnspecified) && ((chromosome_.mutation_rates_M_.size() != 0) || (chromosome_.mutation_rates_F_.size() != 0))) ||
			((requested_sex != IndividualSex::kUnspecified) && (chromosome_.mutation_rates_H_.size() != 0)))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() cannot change the chromosome between using a single map versus separate maps for the sexes; the original configuration must be preserved." << eidos_terminate();
		
		if (((requested_sex == IndividualSex::kUnspecified) && (num_mutation_rates_ > 0)) || ((requested_sex != IndividualSex::kUnspecified) && (num_mutation_rates_ > 1)))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() may be called only once (or once per sex, with sex-specific mutation maps).  The multiple mutation regions of a mutation map must be set up in a single call to initializeMutationRate()." << eidos_terminate();
		
		// Set up to replace the requested map
		vector<slim_position_t> &positions = ((requested_sex == IndividualSex::kUnspecified) ? chromosome_.mutation_end_positions_H_ : 
											  ((requested_sex == IndividualSex::kMale) ? chromosome_.mutation_end_positions_M_ : chromosome_.mutation_end_positions_F_));
		vector<double> &rates = ((requested_sex == IndividualSex::kUnspecified) ? chromosome_.mutation_rates_H_ : 
								 ((requested_sex == IndividualSex::kMale) ? chromosome_.mutation_rates_M_ : chromosome_.mutation_rates_F_));
		
		if (arg1_value->Type() == EidosValueType::kValueNULL)
		{
			if (rate_count != 1)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() requires rates to be a singleton if ends is not supplied." << eidos_terminate();
			
			double mutation_rate = arg0_value->FloatAtIndex(0, nullptr);
			
			// check values
			if (mutation_rate < 0.0)		// intentionally no upper bound
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() requires rates to be >= 0 (" << mutation_rate << " supplied)." << eidos_terminate();
			
			// then adopt them
			rates.clear();
			positions.clear();
			
			rates.emplace_back(mutation_rate);
			//positions.emplace_back(?);	// deferred; patched in Chromosome::InitializeDraws().
		}
		else
		{
			int end_count = arg1_value->Count();
			
			if ((end_count != rate_count) || (end_count == 0))
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() requires ends and rates to be of equal and nonzero size." << eidos_terminate();
			
			// check values
			for (int value_index = 0; value_index < end_count; ++value_index)
			{
				double mutation_rate = arg0_value->FloatAtIndex(value_index, nullptr);
				slim_position_t mutation_end_position = SLiMCastToPositionTypeOrRaise(arg1_value->IntAtIndex(value_index, nullptr));
				
				if (value_index > 0)
					if (mutation_end_position <= arg1_value->IntAtIndex(value_index - 1, nullptr))
						EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() requires ends to be in strictly ascending order." << eidos_terminate();
				
				if (mutation_rate < 0.0)		// intentionally no upper bound
					EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeMutationRate() requires rates to be >= 0 (" << mutation_rate << " supplied)." << eidos_terminate();
			}
			
			// then adopt them
			rates.clear();
			positions.clear();
			
			for (int interval_index = 0; interval_index < end_count; ++interval_index)
			{
				double mutation_rate = arg0_value->FloatAtIndex(interval_index, nullptr);
				slim_position_t mutation_end_position = SLiMCastToPositionTypeOrRaise(arg1_value->IntAtIndex(interval_index, nullptr));
				
				rates.emplace_back(mutation_rate);
				positions.emplace_back(mutation_end_position);
			}
		}
		
		chromosome_changed_ = true;
		
		if (DEBUG_INPUT)
		{
			int ratesSize = (int)rates.size();
			int endsSize = (int)positions.size();
			
			output_stream << "initializeMutationRate(";
			
			if (ratesSize > 1)
				output_stream << "c(";
			for (int interval_index = 0; interval_index < ratesSize; ++interval_index)
				output_stream << (interval_index == 0 ? "" : ", ") << rates[interval_index];
			if (ratesSize > 1)
				output_stream << ")";
			
			if (endsSize > 0)
			{
				output_stream << ", ";
				
				if (endsSize > 1)
					output_stream << "c(";
				for (int interval_index = 0; interval_index < endsSize; ++interval_index)
					output_stream << (interval_index == 0 ? "" : ", ") << positions[interval_index];
				if (endsSize > 1)
					output_stream << ")";
			}
			
			output_stream << ");" << endl;
		}
		
		num_mutation_rates_++;
	}
	
	
	//
	//	*********************	(void)initializeSex(string$ chromosomeType, [numeric$ xDominanceCoeff = 1])
	//
	#pragma mark initializeSex()
	
	else if (p_function_name.compare(gStr_initializeSex) == 0)
	{
		if (num_sex_declarations_ > 0)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeSex() may be called only once." << eidos_terminate();
		
		string chromosome_type = arg0_value->StringAtIndex(0, nullptr);
		
		if (chromosome_type.compare(gStr_A) == 0)
			modeled_chromosome_type_ = GenomeType::kAutosome;
		else if (chromosome_type.compare(gStr_X) == 0)
			modeled_chromosome_type_ = GenomeType::kXChromosome;
		else if (chromosome_type.compare(gStr_Y) == 0)
			modeled_chromosome_type_ = GenomeType::kYChromosome;
		else
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeSex() requires a chromosomeType of \"A\", \"X\", or \"Y\" (\"" << chromosome_type << "\" supplied)." << eidos_terminate();
		
		if (arg1_value->FloatAtIndex(0, nullptr) != 1.0)
		{
			if (modeled_chromosome_type_ == GenomeType::kXChromosome)
				x_chromosome_dominance_coeff_ = arg1_value->FloatAtIndex(0, nullptr);		// intentionally no bounds check
			else
				EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeSex() xDominanceCoeff may be supplied only for chromosomeType \"X\"." << eidos_terminate();
		}
		
		if (DEBUG_INPUT)
		{
			output_stream << "initializeSex(\"" << chromosome_type << "\"";
			
			if (modeled_chromosome_type_ == GenomeType::kXChromosome)
				output_stream << ", " << x_chromosome_dominance_coeff_;
			
			output_stream << ");" << endl;
		}
		
		sex_enabled_ = true;
		num_sex_declarations_++;
	}
	
	
	//
	//	*********************	(void)initializeSLiMOptions([logical$ keepPedigrees = F], [string$ dimensionality = ""], [integer$ mutationRuns = 0], [logical$ preventIncidentalSelfing = F])
	//
	#pragma mark initializeSLiMOptions()
	
	else if (p_function_name.compare(gStr_initializeSLiMOptions) == 0)
	{
#ifdef __clang_analyzer__
		assert(p_argument_count == 4);
#endif
		
		if (num_options_declarations_ > 0)
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeSLiMOptions() may be called only once." << eidos_terminate();
		
		if ((num_interaction_types_ > 0) || (num_mutation_types_ > 0) || (num_mutation_rates_ > 0) || (num_genomic_element_types_ > 0) || (num_genomic_elements_ > 0) || (num_recombination_rates_ > 0) || (num_gene_conversions_ > 0) || (num_sex_declarations_ > 0))
			EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): initializeSLiMOptions() must be called before all other initialization functions." << eidos_terminate();
		
		{
			// [logical$ keepPedigrees = F]
			bool keep_pedigrees = arg0_value->LogicalAtIndex(0, nullptr);
		
			pedigrees_enabled_ = keep_pedigrees;
		}
		
		{
			// [string$ dimensionality = ""]
			std::string space = arg1_value->StringAtIndex(0, nullptr);
			
			if (space.length() != 0)
			{
				if (space == "x")
					spatial_dimensionality_ = 1;
				else if (space == "xy")
					spatial_dimensionality_ = 2;
				else if (space == "xyz")
					spatial_dimensionality_ = 3;
				else
					EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): in initializeSLiMOptions(), legal non-empty values for parameter dimensionality are only 'x', 'xy', and 'xyz'." << eidos_terminate();
			}
		}
		
		{
			// [integer$ mutationRuns = 0]
			int64_t mutrun_count = arg2_value->IntAtIndex(0, nullptr);
			
			if (mutrun_count != 0)
			{
				if ((mutrun_count < 1) || (mutrun_count > 10000))
					EIDOS_TERMINATION << "ERROR (SLiMSim::ContextDefinedFunctionDispatch): in initializeSLiMOptions(), parameter mutationRuns currently must be between 1 and 10000, inclusive." << eidos_terminate();
				
				preferred_mutrun_count_ = (int)mutrun_count;
			}
		}
		
		{
			// [logical$ preventIncidentalSelfing = F]
			bool prevent_selfing = arg3_value->LogicalAtIndex(0, nullptr);
			
			prevent_incidental_selfing = prevent_selfing;
		}
		
		if (DEBUG_INPUT)
		{
			output_stream << "initializeSLiMOptions(";
			
			bool previous_params = false;
			
			if (pedigrees_enabled_)
			{
				if (previous_params) output_stream << ", ";
				output_stream << "keepPedigrees = " << (pedigrees_enabled_ ? "T" : "F");
				previous_params = true;
			}
			
			if (spatial_dimensionality_ != 0)
			{
				if (previous_params) output_stream << ", ";
				output_stream << "dimensionality = ";
				
				if (spatial_dimensionality_ == 1) output_stream << "'x'";
				else if (spatial_dimensionality_ == 2) output_stream << "'xy'";
				else if (spatial_dimensionality_ == 3) output_stream << "'xyz'";
				
				previous_params = true;
			}
			
			if (preferred_mutrun_count_)
			{
				if (previous_params) output_stream << ", ";
				output_stream << "mutationRunCount = " << preferred_mutrun_count_;
				previous_params = true;
			}
			
			if (prevent_incidental_selfing)
			{
				if (previous_params) output_stream << ", ";
				output_stream << "preventIncidentalSelfing = " << (prevent_incidental_selfing ? "T" : "F");
				previous_params = true;
			}
			
			output_stream << ");" << endl;
		}
		
		num_options_declarations_++;
	}
	
	// the initialize...() functions all return invisible NULL
	return gStaticEidosValueNULLInvisible;
}

const std::vector<const EidosFunctionSignature*> *SLiMSim::ZeroGenerationFunctionSignatures(void)
{
	// Allocate our own EidosFunctionSignature objects
	static std::vector<const EidosFunctionSignature*> sim_0_signatures_;
	
	if (!sim_0_signatures_.size())
	{
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeGenomicElement, nullptr, kEidosValueMaskNULL, "SLiM"))
										->AddIntObject_S("genomicElementType", gSLiM_GenomicElementType_Class)->AddInt_S("start")->AddInt_S("end"));
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeGenomicElementType, nullptr, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_GenomicElementType_Class, "SLiM"))
										->AddIntString_S("id")->AddIntObject("mutationTypes", gSLiM_MutationType_Class)->AddNumeric("proportions"));
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeInteractionType, nullptr, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_InteractionType_Class, "SLiM"))
										->AddIntString_S("id")->AddString_S(gStr_spatiality)->AddLogical_OS(gStr_reciprocal, gStaticEidosValue_LogicalF)->AddNumeric_OS(gStr_maxDistance, gStaticEidosValue_FloatINF)->AddString_OS(gStr_sexSegregation, gStaticEidosValue_StringDoubleAsterisk));
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeMutationType, nullptr, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_MutationType_Class, "SLiM"))
										->AddIntString_S("id")->AddNumeric_S("dominanceCoeff")->AddString_S("distributionType")->AddEllipsis());
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeRecombinationRate, nullptr, kEidosValueMaskNULL, "SLiM"))
										->AddNumeric("rates")->AddInt_ON("ends", gStaticEidosValueNULL)->AddString_OS("sex", gStaticEidosValue_StringAsterisk));
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeGeneConversion, nullptr, kEidosValueMaskNULL, "SLiM"))
										->AddNumeric_S("conversionFraction")->AddNumeric_S("meanLength"));
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeMutationRate, nullptr, kEidosValueMaskNULL, "SLiM"))
										->AddNumeric("rates")->AddInt_ON("ends", gStaticEidosValueNULL)->AddString_OS("sex", gStaticEidosValue_StringAsterisk));
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeSex, nullptr, kEidosValueMaskNULL, "SLiM"))
										->AddString_S("chromosomeType")->AddNumeric_OS("xDominanceCoeff", gStaticEidosValue_Float1));
		sim_0_signatures_.emplace_back((EidosFunctionSignature *)(new EidosFunctionSignature(gStr_initializeSLiMOptions, nullptr, kEidosValueMaskNULL, "SLiM"))
										->AddLogical_OS("keepPedigrees", gStaticEidosValue_LogicalF)->AddString_OS("dimensionality", gStaticEidosValue_StringEmpty)->AddInt_OS("mutationRuns", gStaticEidosValue_Integer0)->AddLogical_OS("preventIncidentalSelfing", gStaticEidosValue_LogicalF));
	}
	
	return &sim_0_signatures_;
}

void SLiMSim::AddZeroGenerationFunctionsToMap(EidosFunctionMap &p_map)
{
	const std::vector<const EidosFunctionSignature*> *signatures = ZeroGenerationFunctionSignatures();
	
	if (signatures)
	{
		for (const EidosFunctionSignature *signature : *signatures)
			p_map.insert(EidosFunctionMapPair(signature->call_name_, signature));
	}
}

void SLiMSim::RemoveZeroGenerationFunctionsFromMap(EidosFunctionMap &p_map)
{
	const std::vector<const EidosFunctionSignature*> *signatures = ZeroGenerationFunctionSignatures();
	
	if (signatures)
	{
		for (const EidosFunctionSignature *signature : *signatures)
			p_map.erase(signature->call_name_);
	}
}

const std::vector<const EidosMethodSignature*> *SLiMSim::AllMethodSignatures(void)
{
	static std::vector<const EidosMethodSignature*> *methodSignatures = nullptr;
	
	if (!methodSignatures)
	{
		auto baseMethods =					gEidos_UndefinedClassObject->Methods();
		auto methodsChromosome =			gSLiM_Chromosome_Class->Methods();
		auto methodsGenome =				gSLiM_Genome_Class->Methods();
		auto methodsGenomicElement =		gSLiM_GenomicElement_Class->Methods();
		auto methodsGenomicElementType =	gSLiM_GenomicElementType_Class->Methods();
		auto methodsIndividual =			gSLiM_Individual_Class->Methods();
		auto methodsInteractionType =		gSLiM_InteractionType_Class->Methods();
		auto methodsMutation =				gSLiM_Mutation_Class->Methods();
		auto methodsMutationType =			gSLiM_MutationType_Class->Methods();
		auto methodsSLiMEidosBlock =		gSLiM_SLiMEidosBlock_Class->Methods();
		auto methodsSLiMSim =				gSLiM_SLiMSim_Class->Methods();
		auto methodsSubpopulation =			gSLiM_Subpopulation_Class->Methods();
		auto methodsSubstitution =			gSLiM_Substitution_Class->Methods();
		
		methodSignatures = new std::vector<const EidosMethodSignature*>(*baseMethods);
		
		methodSignatures->insert(methodSignatures->end(), methodsChromosome->begin(), methodsChromosome->end());
		methodSignatures->insert(methodSignatures->end(), methodsGenome->begin(), methodsGenome->end());
		methodSignatures->insert(methodSignatures->end(), methodsGenomicElement->begin(), methodsGenomicElement->end());
		methodSignatures->insert(methodSignatures->end(), methodsGenomicElementType->begin(), methodsGenomicElementType->end());
		methodSignatures->insert(methodSignatures->end(), methodsIndividual->begin(), methodsIndividual->end());
		methodSignatures->insert(methodSignatures->end(), methodsInteractionType->begin(), methodsInteractionType->end());
		methodSignatures->insert(methodSignatures->end(), methodsMutation->begin(), methodsMutation->end());
		methodSignatures->insert(methodSignatures->end(), methodsMutationType->begin(), methodsMutationType->end());
		methodSignatures->insert(methodSignatures->end(), methodsSLiMEidosBlock->begin(), methodsSLiMEidosBlock->end());
		methodSignatures->insert(methodSignatures->end(), methodsSLiMSim->begin(), methodsSLiMSim->end());
		methodSignatures->insert(methodSignatures->end(), methodsSubpopulation->begin(), methodsSubpopulation->end());
		methodSignatures->insert(methodSignatures->end(), methodsSubstitution->begin(), methodsSubstitution->end());
		
		// sort by pointer; we want pointer-identical signatures to end up adjacent
		std::sort(methodSignatures->begin(), methodSignatures->end());
		
		// then unique by pointer value to get a list of unique signatures (which may not be unique by name)
		auto unique_end_iter = std::unique(methodSignatures->begin(), methodSignatures->end());
		methodSignatures->resize(std::distance(methodSignatures->begin(), unique_end_iter));
		
		// print out any signatures that are identical by name
		std::sort(methodSignatures->begin(), methodSignatures->end(), CompareEidosCallSignatures);
		
		const EidosMethodSignature *previous_sig = nullptr;
		
		for (const EidosMethodSignature *sig : *methodSignatures)
		{
			if (previous_sig && (sig->call_name_.compare(previous_sig->call_name_) == 0))
			{
				// We have a name collision.  That is OK as long as the method signatures are identical.
				if ((typeid(*sig) != typeid(*previous_sig)) ||
					(sig->is_class_method != previous_sig->is_class_method) ||
					(sig->call_name_ != previous_sig->call_name_) ||
					(sig->return_mask_ != previous_sig->return_mask_) ||
					(sig->return_class_ != previous_sig->return_class_) ||
					(sig->arg_masks_ != previous_sig->arg_masks_) ||
					(sig->arg_names_ != previous_sig->arg_names_) ||
					(sig->arg_classes_ != previous_sig->arg_classes_) ||
					(sig->has_optional_args_ != previous_sig->has_optional_args_) ||
					(sig->has_ellipsis_ != previous_sig->has_ellipsis_))
					std::cout << "Duplicate method name with a different signature: " << sig->call_name_ << std::endl;
			}
			
			previous_sig = sig;
		}
		
		// log a full list
		//std::cout << "----------------" << std::endl;
		//for (const EidosMethodSignature *sig : *methodSignatures)
		//	std::cout << sig->call_name_ << " (" << sig << ")" << std::endl;
	}
	
	return methodSignatures;
}

const std::vector<const EidosPropertySignature*> *SLiMSim::AllPropertySignatures(void)
{
	static std::vector<const EidosPropertySignature*> *propertySignatures = nullptr;
	
	if (!propertySignatures)
	{
		auto baseProperties =					gEidos_UndefinedClassObject->Properties();
		auto propertiesChromosome =				gSLiM_Chromosome_Class->Properties();
		auto propertiesGenome =					gSLiM_Genome_Class->Properties();
		auto propertiesGenomicElement =			gSLiM_GenomicElement_Class->Properties();
		auto propertiesGenomicElementType =		gSLiM_GenomicElementType_Class->Properties();
		auto propertiesIndividual =				gSLiM_Individual_Class->Properties();
		auto propertiesInteractionType =		gSLiM_InteractionType_Class->Properties();
		auto propertiesMutation =				gSLiM_Mutation_Class->Properties();
		auto propertiesMutationType =			gSLiM_MutationType_Class->Properties();
		auto propertiesSLiMEidosBlock =			gSLiM_SLiMEidosBlock_Class->Properties();
		auto propertiesSLiMSim =				gSLiM_SLiMSim_Class->Properties();
		auto propertiesSubpopulation =			gSLiM_Subpopulation_Class->Properties();
		auto propertiesSubstitution =			gSLiM_Substitution_Class->Properties();
		
		propertySignatures = new std::vector<const EidosPropertySignature*>(*baseProperties);
		
		propertySignatures->insert(propertySignatures->end(), propertiesChromosome->begin(), propertiesChromosome->end());
		propertySignatures->insert(propertySignatures->end(), propertiesGenome->begin(), propertiesGenome->end());
		propertySignatures->insert(propertySignatures->end(), propertiesGenomicElement->begin(), propertiesGenomicElement->end());
		propertySignatures->insert(propertySignatures->end(), propertiesGenomicElementType->begin(), propertiesGenomicElementType->end());
		propertySignatures->insert(propertySignatures->end(), propertiesIndividual->begin(), propertiesIndividual->end());
		propertySignatures->insert(propertySignatures->end(), propertiesInteractionType->begin(), propertiesInteractionType->end());
		propertySignatures->insert(propertySignatures->end(), propertiesMutation->begin(), propertiesMutation->end());
		propertySignatures->insert(propertySignatures->end(), propertiesMutationType->begin(), propertiesMutationType->end());
		propertySignatures->insert(propertySignatures->end(), propertiesSLiMEidosBlock->begin(), propertiesSLiMEidosBlock->end());
		propertySignatures->insert(propertySignatures->end(), propertiesSLiMSim->begin(), propertiesSLiMSim->end());
		propertySignatures->insert(propertySignatures->end(), propertiesSubpopulation->begin(), propertiesSubpopulation->end());
		propertySignatures->insert(propertySignatures->end(), propertiesSubstitution->begin(), propertiesSubstitution->end());
		
		// sort by pointer; we want pointer-identical signatures to end up adjacent
		std::sort(propertySignatures->begin(), propertySignatures->end());
		
		// then unique by pointer value to get a list of unique signatures (which may not be unique by name)
		auto unique_end_iter = std::unique(propertySignatures->begin(), propertySignatures->end());
		propertySignatures->resize(std::distance(propertySignatures->begin(), unique_end_iter));
		
		// print out any signatures that are identical by name
		std::sort(propertySignatures->begin(), propertySignatures->end(), CompareEidosPropertySignatures);
		
		const EidosPropertySignature *previous_sig = nullptr;
		
		for (const EidosPropertySignature *sig : *propertySignatures)
		{
			if (previous_sig && (sig->property_name_.compare(previous_sig->property_name_) == 0))
			{
				// We have a name collision.  That is OK as long as the property signatures are identical.
				if ((sig->property_id_ != previous_sig->property_id_) ||
					(sig->read_only_ != previous_sig->read_only_) ||
					(sig->value_mask_ != previous_sig->value_mask_) ||
					(sig->value_class_ != previous_sig->value_class_))
					std::cout << "Duplicate property name with different signature: " << sig->property_name_ << std::endl;
			}
			
			previous_sig = sig;
		}
	}
	
	return propertySignatures;
}

EidosSymbolTable *SLiMSim::SymbolsFromBaseSymbols(EidosSymbolTable *p_base_symbols)
{
	// Since we keep our own symbol table long-term, this function does not actually re-derive a new table, but just returns the cached table
	if (p_base_symbols != gEidosConstantsSymbolTable)
		EIDOS_TERMINATION << "ERROR (SLiMSim::SymbolsFromBaseSymbols): (internal error) SLiM requires that its parent symbol table be the standard Eidos symbol table." << eidos_terminate();
	
	return simulation_constants_;
}

const EidosObjectClass *SLiMSim::Class(void) const
{
	return gSLiM_SLiMSim_Class;
}

EidosValue_SP SLiMSim::GetProperty(EidosGlobalStringID p_property_id)
{
	// All of our strings are in the global registry, so we can require a successful lookup
	switch (p_property_id)
	{
			// constants
		case gID_chromosome:
			return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Object_singleton(&chromosome_, gSLiM_Chromosome_Class));
		case gID_chromosomeType:
		{
			switch (modeled_chromosome_type_)
			{
				case GenomeType::kAutosome:		return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_String_singleton(gStr_A));
				case GenomeType::kXChromosome:	return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_String_singleton(gStr_X));
				case GenomeType::kYChromosome:	return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_String_singleton(gStr_Y));
			}
		}
		case gID_dimensionality:
		{
			static EidosValue_SP static_dimensionality_string_x;
			static EidosValue_SP static_dimensionality_string_xy;
			static EidosValue_SP static_dimensionality_string_xyz;
			
			if (!static_dimensionality_string_x)
			{
				static_dimensionality_string_x = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_String_singleton(gEidosStr_x));
				static_dimensionality_string_xy = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_String_singleton("xy"));
				static_dimensionality_string_xyz = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_String_singleton("xyz"));
			}
			
			switch (spatial_dimensionality_)
			{
				case 0:		return gStaticEidosValue_StringEmpty;
				case 1:		return static_dimensionality_string_x;
				case 2:		return static_dimensionality_string_xy;
				case 3:		return static_dimensionality_string_xyz;
			}
		}
		case gID_genomicElementTypes:
		{
			EidosValue_Object_vector *vec = new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_GenomicElementType_Class);
			EidosValue_SP result_SP = EidosValue_SP(vec);
			
			for (auto ge_type = genomic_element_types_.begin(); ge_type != genomic_element_types_.end(); ++ge_type)
				vec->PushObjectElement(ge_type->second);
			
			return result_SP;
		}
		case gID_inSLiMgui:
		{
#ifdef SLIMGUI
			return gStaticEidosValue_LogicalT;
#else
			return gStaticEidosValue_LogicalF;
#endif
		}
		case gID_interactionTypes:
		{
			EidosValue_Object_vector *vec = new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_InteractionType_Class);
			EidosValue_SP result_SP = EidosValue_SP(vec);
			
			for (auto int_type = interaction_types_.begin(); int_type != interaction_types_.end(); ++int_type)
				vec->PushObjectElement(int_type->second);
			
			return result_SP;
		}
		case gID_mutations:
		{
			Mutation *mut_block_ptr = gSLiM_Mutation_Block;
			MutationRun &mutation_registry = population_.mutation_registry_;
			int mutation_count = mutation_registry.size();
			EidosValue_Object_vector *vec = (new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_Mutation_Class))->Reserve(mutation_count);
			EidosValue_SP result_SP = EidosValue_SP(vec);
			
			for (int mut_index = 0; mut_index < mutation_count; ++mut_index)
				vec->PushObjectElement(mut_block_ptr + mutation_registry[mut_index]);
			
			return result_SP;
		}
		case gID_mutationTypes:
		{
			EidosValue_Object_vector *vec = new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_MutationType_Class);
			EidosValue_SP result_SP = EidosValue_SP(vec);
			
			for (auto mutation_type = mutation_types_.begin(); mutation_type != mutation_types_.end(); ++mutation_type)
				vec->PushObjectElement(mutation_type->second);
			
			return result_SP;
		}
		case gID_scriptBlocks:
		{
			EidosValue_Object_vector *vec = new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_SLiMEidosBlock_Class);
			EidosValue_SP result_SP = EidosValue_SP(vec);
			std::vector<SLiMEidosBlock*> &script_blocks = AllScriptBlocks();
			
			for (auto script_block = script_blocks.begin(); script_block != script_blocks.end(); ++script_block)
				if ((*script_block)->type_ != SLiMEidosBlockType::SLiMEidosUserDefinedFunction)		// exclude function blocks; not user-visible
					vec->PushObjectElement(*script_block);
			
			return result_SP;
		}
		case gID_sexEnabled:
			return (sex_enabled_ ? gStaticEidosValue_LogicalT : gStaticEidosValue_LogicalF);
		case gID_subpopulations:
		{
			EidosValue_Object_vector *vec = new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_Subpopulation_Class);
			EidosValue_SP result_SP = EidosValue_SP(vec);
			
			for (auto pop = population_.begin(); pop != population_.end(); ++pop)
				vec->PushObjectElement(pop->second);
			
			return result_SP;
		}
		case gID_substitutions:
		{
			EidosValue_Object_vector *vec = new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_Substitution_Class);
			EidosValue_SP result_SP = EidosValue_SP(vec);
			
			for (auto sub_iter = population_.substitutions_.begin(); sub_iter != population_.substitutions_.end(); ++sub_iter)
				vec->PushObjectElement(*sub_iter);
			
			return result_SP;
		}
			
			// variables
		case gID_dominanceCoeffX:
			return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Float_singleton(x_chromosome_dominance_coeff_));
		case gID_generation:
		{
			if (!cached_value_generation_)
				cached_value_generation_ = EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Int_singleton(generation_));
			return cached_value_generation_;
		}
		case gID_tag:
			return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Int_singleton(tag_value_));
			
			// all others, including gID_none
		default:
			return EidosObjectElement::GetProperty(p_property_id);
	}
}

void SLiMSim::SetProperty(EidosGlobalStringID p_property_id, const EidosValue &p_value)
{
	// All of our strings are in the global registry, so we can require a successful lookup
	switch (p_property_id)
	{
		case gID_generation:
		{
			int64_t value = p_value.IntAtIndex(0, nullptr);
			slim_generation_t old_generation = generation_;
			
			generation_ = SLiMCastToGenerationTypeOrRaise(value);
			cached_value_generation_.reset();
			
			// Setting the generation into the future is generally harmless; the simulation logic is designed to handle that anyway, since
			// that happens every generation.  Setting the generation into the past is a bit tricker, since some things that have already
			// occurred need to be invalidated.  In particular, historical data cached by SLiMgui needs to be fixed.  Note that here we
			// do NOT remove Substitutions that are in the future, or otherwise try to backtrack the actual simulation state.  If the user
			// actually restores a past state with readFromPopulationFile(), all that kind of stuff will be reset; but if all they do is
			// set the generation counter back, the model state is unchanged, substitutions are still fixed, etc.  This means that the
			// simulation code needs to be robust to the possibility that some records, e.g. for Substitutions, may appear to be about
			// events in the future.  But usually users will only set the generation back if they also call readFromPopulationFile().
			if (generation_ < old_generation)
			{
#ifdef SLIMGUI
				// Fix fitness histories for SLiMgui.  Note that mutation_loss_times_ and mutation_fixation_times_ are not fixable, since
				// their entries are not separated out by generation, so we just leave them as is, containing information about
				// alternative futures of the model.
				for (auto history_record_iter : population_.fitness_histories_)
				{
					FitnessHistory &history_record = history_record_iter.second;
					double *history = history_record.history_;
					
					if (history)
					{
						int old_last_valid_history_index = std::max(0, old_generation - 2);		// if gen==2, gen 1 was the last valid entry, and it is at index 0
						int new_last_valid_history_index = std::max(0, generation_ - 2);		// ditto
						
						// make sure that we don't overrun the end of the buffer
						if (old_last_valid_history_index > history_record.history_length_ - 1)
							old_last_valid_history_index = history_record.history_length_ - 1;
						
						for (int entry_index = new_last_valid_history_index + 1; entry_index <= old_last_valid_history_index; ++entry_index)
							history[entry_index] = NAN;
					}
				}
#endif
			}
			
			return;
		}
			
		case gID_dominanceCoeffX:
		{
			if (!sex_enabled_ || (modeled_chromosome_type_ != GenomeType::kXChromosome))
				EIDOS_TERMINATION << "ERROR (SLiMSim::SetProperty): attempt to set property dominanceCoeffX when not simulating an X chromosome." << eidos_terminate();
			
			double value = p_value.FloatAtIndex(0, nullptr);
			
			x_chromosome_dominance_coeff_ = value;		// intentionally no bounds check
			return;
		}
			
		case gID_tag:
		{
			slim_usertag_t value = SLiMCastToUsertagTypeOrRaise(p_value.IntAtIndex(0, nullptr));
			
			tag_value_ = value;
			return;
		}
			
			// all others, including gID_none
		default:
			return EidosObjectElement::SetProperty(p_property_id, p_value);
	}
}

EidosValue_SP SLiMSim::ExecuteInstanceMethod(EidosGlobalStringID p_method_id, const EidosValue_SP *const p_arguments, int p_argument_count, EidosInterpreter &p_interpreter)
{
	EidosValue *arg0_value = ((p_argument_count >= 1) ? p_arguments[0].get() : nullptr);
	EidosValue *arg1_value = ((p_argument_count >= 2) ? p_arguments[1].get() : nullptr);
	EidosValue *arg2_value = ((p_argument_count >= 3) ? p_arguments[2].get() : nullptr);
	EidosValue *arg3_value = ((p_argument_count >= 4) ? p_arguments[3].get() : nullptr);
	EidosValue *arg4_value = ((p_argument_count >= 5) ? p_arguments[4].get() : nullptr);
	EidosValue *arg5_value = ((p_argument_count >= 6) ? p_arguments[5].get() : nullptr);
	
	
	// All of our strings are in the global registry, so we can require a successful lookup
	switch (p_method_id)
	{
			//
			//	*********************	– (object<Subpopulation>$)addSubpop(is$ subpopID, integer$ size, [float$ sexRatio = 0.5])
			//
#pragma mark -addSubpop()
			
		case gID_addSubpop:
		{
			slim_objectid_t subpop_id = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 'p', nullptr);
			slim_popsize_t subpop_size = SLiMCastToPopsizeTypeOrRaise(arg1_value->IntAtIndex(0, nullptr));
			
			double sex_ratio = arg2_value->FloatAtIndex(0, nullptr);
			
			if ((sex_ratio != 0.5) && !sex_enabled_)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): addSubpop() sex ratio supplied in non-sexual simulation." << eidos_terminate();
			
			// construct the subpop; we always pass the sex ratio, but AddSubpopulation will not use it if sex is not enabled, for simplicity
			Subpopulation *new_subpop = population_.AddSubpopulation(subpop_id, subpop_size, sex_ratio);
			
			// define a new Eidos variable to refer to the new subpopulation
			EidosSymbolTableEntry &symbol_entry = new_subpop->SymbolTableEntry();
			
			if (p_interpreter.SymbolTable().ContainsSymbol(symbol_entry.first))
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): addSubpop() symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate();
			
			simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
			
			return symbol_entry.second;
		}
			
			
			//
			//	*********************	– (object<Subpopulation>$)addSubpopSplit(is$ subpopID, integer$ size, io<Subpopulation>$ sourceSubpop, [float$ sexRatio = 0.5])
			//
#pragma mark -addSubpopSplit()
			
		case gID_addSubpopSplit:
		{
			slim_objectid_t subpop_id = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 'p', nullptr);
			slim_popsize_t subpop_size = SLiMCastToPopsizeTypeOrRaise(arg1_value->IntAtIndex(0, nullptr));
			Subpopulation *source_subpop = nullptr;
			
			if (arg2_value->Type() == EidosValueType::kValueInt)
			{
				slim_objectid_t source_subpop_id = SLiMCastToObjectidTypeOrRaise(arg2_value->IntAtIndex(0, nullptr));
				SLiMSim *sim = dynamic_cast<SLiMSim *>(p_interpreter.Context());
				
				if (sim)
				{
					auto found_subpop_pair = sim->ThePopulation().find(source_subpop_id);
					
					if (found_subpop_pair != sim->ThePopulation().end())
						source_subpop = found_subpop_pair->second;
				}
				
				if (!source_subpop)
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): addSubpopSplit() subpopulation p" << source_subpop_id << " not defined." << eidos_terminate();
			}
			else
			{
				source_subpop = dynamic_cast<Subpopulation *>(arg2_value->ObjectElementAtIndex(0, nullptr));
			}
			
			double sex_ratio = arg3_value->FloatAtIndex(0, nullptr);
			
			if ((sex_ratio != 0.5) && !sex_enabled_)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): addSubpopSplit() sex ratio supplied in non-sexual simulation." << eidos_terminate();
			
			// construct the subpop; we always pass the sex ratio, but AddSubpopulation will not use it if sex is not enabled, for simplicity
			Subpopulation *new_subpop = population_.AddSubpopulation(subpop_id, *source_subpop, subpop_size, sex_ratio);
			
			// define a new Eidos variable to refer to the new subpopulation
			EidosSymbolTableEntry &symbol_entry = new_subpop->SymbolTableEntry();
			
			if (p_interpreter.SymbolTable().ContainsSymbol(symbol_entry.first))
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): addSubpopSplit() symbol " << StringForEidosGlobalStringID(symbol_entry.first) << " was already defined prior to its definition here." << eidos_terminate();
			
			simulation_constants_->InitializeConstantSymbolEntry(symbol_entry);
			
			return symbol_entry.second;
		}
			
			
			//
			//	*********************	- (void)deregisterScriptBlock(io<SLiMEidosBlock> scriptBlocks)
			//
#pragma mark -deregisterScriptBlock()
			
		case gID_deregisterScriptBlock:
		{
			int block_count = arg0_value->Count();
			
			// We just schedule the blocks for deregistration; we do not deregister them immediately, because that would leave stale pointers lying around
			for (int block_index = 0; block_index < block_count; ++block_index)
			{
				SLiMEidosBlock *block = nullptr;
				
				if (arg0_value->Type() == EidosValueType::kValueInt)
				{
					slim_objectid_t block_id = SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(block_index, nullptr));
					std::vector<SLiMEidosBlock*> &script_blocks = AllScriptBlocks();
					
					for (SLiMEidosBlock *found_block : script_blocks)
						if (found_block->block_id_ == block_id)
						{
							block = found_block;
							break;
						}
					
					if (!block)
						EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): deregisterScriptBlock() SLiMEidosBlock s" << block_id << " not defined." << eidos_terminate();
				}
				else
				{
					block = dynamic_cast<SLiMEidosBlock *>(arg0_value->ObjectElementAtIndex(block_index, nullptr));
				}
				
				if (block->type_ == SLiMEidosBlockType::SLiMEidosUserDefinedFunction)
				{
					// this should never be hit, because the user should have to way to get a reference to a function block
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): (internal error) deregisterScriptBlock() cannot be called on user-defined function script blocks." << eidos_terminate();
				}
				else if (block->type_ == SLiMEidosBlockType::SLiMEidosInteractionCallback)
				{
					// interaction() callbacks have to work differently, because they can be called at any time after an
					// interaction has been evaluated, up until the interaction is invalidated; we can't make pointers
					// to interaction() callbacks go stale except at that specific point in the generation cycle
					if (std::find(scheduled_interaction_deregs_.begin(), scheduled_interaction_deregs_.end(), block) != scheduled_interaction_deregs_.end())
						EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): deregisterScriptBlock() called twice on the same script block." << eidos_terminate();
					
					scheduled_interaction_deregs_.emplace_back(block);
				}
				else
				{
					// all other script blocks go on the main list and get cleared out at the end of each generation stage
					if (std::find(scheduled_deregistrations_.begin(), scheduled_deregistrations_.end(), block) != scheduled_deregistrations_.end())
						EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): deregisterScriptBlock() called twice on the same script block." << eidos_terminate();
					
					scheduled_deregistrations_.emplace_back(block);
				}
			}
			
			return gStaticEidosValueNULLInvisible;
		}
			
			
			//
			//	*********************	– (float)mutationFrequencies(No<Subpopulation> subpops, [No<Mutation> mutations = NULL])
			//	*********************	– (integer)mutationCounts(No<Subpopulation> subpops, [No<Mutation> mutations = NULL])
			//
#pragma mark -mutationFrequencies()
#pragma mark -mutationCounts()
			
		case gID_mutationFrequencies:
		case gID_mutationCounts:
		{
			slim_refcount_t total_genome_count = 0;
			
			// tally across the requested subpops
			if (arg0_value->Type() == EidosValueType::kValueNULL)
			{
				// tally across the whole population
				total_genome_count = population_.TallyMutationReferences(nullptr, false);
			}
			else
			{
				// requested subpops, so get them
				int requested_subpop_count = arg0_value->Count();
				vector<Subpopulation*> subpops_to_tally;
				
				if (requested_subpop_count)
				{
					for (int requested_subpop_index = 0; requested_subpop_index < requested_subpop_count; ++requested_subpop_index)
						subpops_to_tally.emplace_back((Subpopulation *)(arg0_value->ObjectElementAtIndex(requested_subpop_index, nullptr)));
				}
				
				total_genome_count = population_.TallyMutationReferences(&subpops_to_tally, false);
			}
			
			// OK, now construct our result vector from the tallies for just the requested mutations
			slim_refcount_t *refcount_block_ptr = gSLiM_Mutation_Refcounts;
			EidosValue_Float_vector *float_result = nullptr;
			EidosValue_Int_vector *int_result = nullptr;
			EidosValue_SP result_SP;
			double denominator = 1.0 / total_genome_count;

			if (p_method_id == gID_mutationFrequencies)
			{
				float_result = new (gEidosValuePool->AllocateChunk()) EidosValue_Float_vector();
				result_SP = EidosValue_SP(float_result);
			}
			else // p_method_id == gID_mutationCounts
			{
				int_result = new (gEidosValuePool->AllocateChunk()) EidosValue_Int_vector();
				result_SP = EidosValue_SP(int_result);
			}
			
			if (arg1_value->Type() != EidosValueType::kValueNULL)
			{
				// a vector of mutations was given, so loop through them and take their tallies
				int arg1_count = arg1_value->Count();
				
				if (arg1_count)
				{
					if (p_method_id == gID_mutationFrequencies)
					{
						for (int value_index = 0; value_index < arg1_count; ++value_index)
						{
							Mutation *mut = (Mutation *)(arg1_value->ObjectElementAtIndex(value_index, nullptr));
							MutationIndex mut_index = mut->BlockIndex();
							
							float_result->PushFloat(*(refcount_block_ptr + mut_index) * denominator);
						}
					}
					else // p_method_id == gID_mutationCounts
					{
						for (int value_index = 0; value_index < arg1_count; ++value_index)
						{
							Mutation *mut = (Mutation *)(arg1_value->ObjectElementAtIndex(value_index, nullptr));
							MutationIndex mut_index = mut->BlockIndex();
							
							int_result->PushInt(*(refcount_block_ptr + mut_index));
						}
					}
				}
			}
			else
			{
				// no mutation vector was given, so return all frequencies from the registry
				MutationIndex *registry_iter_end = population_.mutation_registry_.end_pointer();
				
				if (p_method_id == gID_mutationFrequencies)
				{
					for (const MutationIndex *registry_iter = population_.mutation_registry_.begin_pointer_const(); registry_iter != registry_iter_end; ++registry_iter)
						float_result->PushFloat(*(refcount_block_ptr + *registry_iter) * denominator);
				}
				else // p_method_id == gID_mutationCounts
				{
					for (const MutationIndex *registry_iter = population_.mutation_registry_.begin_pointer_const(); registry_iter != registry_iter_end; ++registry_iter)
						int_result->PushInt(*(refcount_block_ptr + *registry_iter));
				}
			}
			
			return result_SP;
		}
			
			
			//
			//	*********************	- (object<Mutation>)mutationsOfType(io<MutationType>$ mutType)
			//
#pragma mark -mutationsOfType()
			
		case gID_mutationsOfType:
		{
			MutationType *mutation_type_ptr = nullptr;
			
			if (arg0_value->Type() == EidosValueType::kValueInt)
			{
				slim_objectid_t mutation_type_id = SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr));
				auto found_muttype_pair = mutation_types_.find(mutation_type_id);
				
				if (found_muttype_pair == mutation_types_.end())
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): mutationsOfType() mutation type m" << mutation_type_id << " not defined." << eidos_terminate();
				
				mutation_type_ptr = found_muttype_pair->second;
			}
			else
			{
				mutation_type_ptr = (MutationType *)(arg0_value->ObjectElementAtIndex(0, nullptr));
			}
			
			// Count the number of mutations of the given type, so we can reserve the right vector size
			// To avoid having to scan the registry twice for the simplest case of a single mutation, we cache the first mutation found
			Mutation *mut_block_ptr = gSLiM_Mutation_Block;
			MutationRun &mutation_registry = population_.mutation_registry_;
			int mutation_count = mutation_registry.size();
			int match_count = 0, mut_index;
			MutationIndex first_match = -1;
			
			for (mut_index = 0; mut_index < mutation_count; ++mut_index)
			{
				MutationIndex mut = mutation_registry[mut_index];
				
				if ((mut_block_ptr + mut)->mutation_type_ptr_ == mutation_type_ptr)
				{
					if (++match_count == 1)
						first_match = mut;
				}
			}
			
			// Now allocate the result vector and assemble it
			if (match_count == 1)
			{
				return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Object_singleton(mut_block_ptr + first_match, gSLiM_Mutation_Class));
			}
			else
			{
				EidosValue_Object_vector *vec = (new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_Mutation_Class))->Reserve(match_count);
				EidosValue_SP result_SP = EidosValue_SP(vec);
				
				if (match_count != 0)
				{
					for (mut_index = 0; mut_index < mutation_count; ++mut_index)
					{
						MutationIndex mut = mutation_registry[mut_index];
						
						if ((mut_block_ptr + mut)->mutation_type_ptr_ == mutation_type_ptr)
							vec->PushObjectElement(mut_block_ptr + mut);
					}
				}
				
				return result_SP;
			}
		}
			
			
			//
			//	*********************	- (integer$)countOfMutationsOfType(io<MutationType>$ mutType)
			//
#pragma mark -countOfMutationsOfType()
			
		case gID_countOfMutationsOfType:
		{
			MutationType *mutation_type_ptr = nullptr;
			
			if (arg0_value->Type() == EidosValueType::kValueInt)
			{
				slim_objectid_t mutation_type_id = SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr));
				auto found_muttype_pair = mutation_types_.find(mutation_type_id);
				
				if (found_muttype_pair == mutation_types_.end())
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): countOfMutationsOfType() mutation type m" << mutation_type_id << " not defined." << eidos_terminate();
				
				mutation_type_ptr = found_muttype_pair->second;
			}
			else
			{
				mutation_type_ptr = (MutationType *)(arg0_value->ObjectElementAtIndex(0, nullptr));
			}
			
			// Count the number of mutations of the given type
			Mutation *mut_block_ptr = gSLiM_Mutation_Block;
			MutationRun &mutation_registry = population_.mutation_registry_;
			int mutation_count = mutation_registry.size();
			int match_count = 0, mut_index;
			
			for (mut_index = 0; mut_index < mutation_count; ++mut_index)
				if ((mut_block_ptr + mutation_registry[mut_index])->mutation_type_ptr_ == mutation_type_ptr)
					++match_count;
			
			return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Int_singleton(match_count));
		}
			
			
			//
			//	*********************	– (void)outputFixedMutations([Ns$ filePath = NULL], [logical$ append=F])
			//
#pragma mark -outputFixedMutations()
			
		case gID_outputFixedMutations:
		{
			std::ostringstream &output_stream = p_interpreter.ExecutionOutputStream();
			
			if ((GenerationStage() == SLiMGenerationStage::kStage1ExecuteEarlyScripts) && (!warned_early_output_))
			{
				output_stream << "#WARNING (SLiMSim::ExecuteInstanceMethod): outputFixedMutations() should probably not be called from an early() event; the output will reflect state at the beginning of the generation, not the end." << std::endl;
				warned_early_output_ = true;
			}
			
			std::ofstream outfile;
			bool has_file = false;
			string outfile_path;
			
			if (arg0_value->Type() != EidosValueType::kValueNULL)
			{
				outfile_path = EidosResolvedPath(arg0_value->StringAtIndex(0, nullptr));
				bool append = arg1_value->LogicalAtIndex(0, nullptr);
				
				outfile.open(outfile_path.c_str(), append ? (std::ios_base::app | std::ios_base::out) : std::ios_base::out);
				has_file = true;
				
				if (!outfile.is_open())
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): outputFixedMutations() could not open "<< outfile_path << "." << eidos_terminate();
			}
			
			std::ostream &out = *(has_file ? dynamic_cast<std::ostream *>(&outfile) : dynamic_cast<std::ostream *>(&output_stream));
			
#if DO_MEMORY_CHECKS
			// This method can burn a huge amount of memory and get us killed, if we have a maximum memory usage.  It's nice to
			// try to check for that and terminate with a proper error message, to help the user diagnose the problem.
			int mem_check_counter = 0, mem_check_mod = 100;
			
			if (eidos_do_memory_checks)
				EidosCheckRSSAgainstMax("SLiMSim::ExecuteInstanceMethod", "(outputFixedMutations(): The memory usage was already out of bounds on entry.)");
#endif
			
			// Output header line
			out << "#OUT: " << generation_ << " F";
			
			if (has_file)
				out << " " << outfile_path;
			
			out << endl;
			
			// Output Mutations section
			out << "Mutations:" << endl;
			
			std::vector<Substitution*> &subs = population_.substitutions_;
			
			for (unsigned int i = 0; i < subs.size(); i++)
			{
				out << i << " ";
				subs[i]->print(out);
				
#if DO_MEMORY_CHECKS
				if (eidos_do_memory_checks && ((++mem_check_counter) % mem_check_mod == 0))
					EidosCheckRSSAgainstMax("SLiMSim::ExecuteInstanceMethod", "(outputFixedMutations(): Out of memory while outputting substitution objects.)");
#endif
			}
			
			if (has_file)
				outfile.close(); 
			
			return gStaticEidosValueNULLInvisible;
		}
			
			
			//
			//	*********************	– (void)outputFull([Ns$ filePath = NULL], [logical$ binary = F], [logical$ append=F], [logical$ spatialPositions = T])
			//
#pragma mark -outputFull()
			
		case gID_outputFull:
		{
			if ((GenerationStage() == SLiMGenerationStage::kStage1ExecuteEarlyScripts) && (!warned_early_output_))
			{
				p_interpreter.ExecutionOutputStream() << "#WARNING (SLiMSim::ExecuteInstanceMethod): outputFull() should probably not be called from an early() event; the output will reflect state at the beginning of the generation, not the end." << std::endl;
				warned_early_output_ = true;
			}
			
			bool use_binary = arg1_value->LogicalAtIndex(0, nullptr);
			bool output_spatial_positions = arg3_value->LogicalAtIndex(0, nullptr);
			
			if (arg0_value->Type() == EidosValueType::kValueNULL)
			{
				if (use_binary)
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): outputFull() cannot output in binary format to the standard output stream; specify a file for output." << eidos_terminate();
				
				std::ostringstream &output_stream = p_interpreter.ExecutionOutputStream();
				
				output_stream << "#OUT: " << generation_ << " A" << endl;
				population_.PrintAll(output_stream, output_spatial_positions);
			}
			else
			{
				string outfile_path = EidosResolvedPath(arg0_value->StringAtIndex(0, nullptr));
				bool append = arg2_value->LogicalAtIndex(0, nullptr);
				std::ofstream outfile;
				
				if (use_binary && append)
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): outputFull() cannot append in binary format." << eidos_terminate();
				
				if (use_binary)
					outfile.open(outfile_path.c_str(), std::ios::out | std::ios::binary);
				else
					outfile.open(outfile_path.c_str(), append ? (std::ios_base::app | std::ios_base::out) : std::ios_base::out);
				
				if (outfile.is_open())
				{
					if (use_binary)
					{
						population_.PrintAllBinary(outfile, output_spatial_positions);
					}
					else
					{
						// We no longer have input parameters to print; possibly this should print all the initialize...() functions called...
						//				const std::vector<std::string> &input_parameters = p_sim.InputParameters();
						//				
						//				for (int i = 0; i < input_parameters.size(); i++)
						//					outfile << input_parameters[i] << endl;
						
						outfile << "#OUT: " << generation_ << " A " << outfile_path << endl;
						population_.PrintAll(outfile, output_spatial_positions);
					}
					
					outfile.close(); 
				}
				else
				{
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): outputFull() could not open "<< outfile_path << "." << eidos_terminate();
				}
			}
			
			return gStaticEidosValueNULLInvisible;
		}
			
			
			//
			//	*********************	– (void)outputMutations(object<Mutation> mutations, [Ns$ filePath = NULL], [logical$ append=F])
			//
#pragma mark -outputMutations()
			
		case gID_outputMutations:
		{
			std::ostringstream &output_stream = p_interpreter.ExecutionOutputStream();
			
			if ((GenerationStage() == SLiMGenerationStage::kStage1ExecuteEarlyScripts) && (!warned_early_output_))
			{
				output_stream << "#WARNING (SLiMSim::ExecuteInstanceMethod): outputMutations() should probably not be called from an early() event; the output will reflect state at the beginning of the generation, not the end." << std::endl;
				warned_early_output_ = true;
			}
			
			std::ofstream outfile;
			bool has_file = false;
			
			if (arg1_value->Type() != EidosValueType::kValueNULL)
			{
				string outfile_path = EidosResolvedPath(arg1_value->StringAtIndex(0, nullptr));
				bool append = arg2_value->LogicalAtIndex(0, nullptr);
				
				outfile.open(outfile_path.c_str(), append ? (std::ios_base::app | std::ios_base::out) : std::ios_base::out);
				has_file = true;
				
				if (!outfile.is_open())
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): outputMutations() could not open "<< outfile_path << "." << eidos_terminate();
			}
			
			std::ostream &out = *(has_file ? (std::ostream *)&outfile : (std::ostream *)&output_stream);
			
			// Extract all of the Mutation objects in mutations; would be nice if there was a simpler way to do this
			EidosValue_Object *mutations_object = (EidosValue_Object *)arg0_value;
			int mutations_count = mutations_object->Count();
			std::vector<Mutation *> mutations;
			
			for (int mutation_index = 0; mutation_index < mutations_count; mutation_index++)
				mutations.emplace_back((Mutation *)(mutations_object->ObjectElementAtIndex(mutation_index, nullptr)));
			
			// find all polymorphisms of the mutations that are to be tracked
			Mutation *mut_block_ptr = gSLiM_Mutation_Block;
			
			if (mutations_count > 0)
			{
				for (const std::pair<const slim_objectid_t,Subpopulation*> &subpop_pair : population_)
				{
					Subpopulation *subpop = subpop_pair.second;
					PolymorphismMap polymorphisms;
					
					for (slim_popsize_t i = 0; i < 2 * subpop->parent_subpop_size_; i++)	// go through all parents
					{
						Genome &genome = subpop->parent_genomes_[i];
						int mutrun_count = genome.mutrun_count_;
						
						for (int run_index = 0; run_index < mutrun_count; ++run_index)
						{
							MutationRun *mutrun = genome.mutruns_[run_index].get();
							int mut_count = mutrun->size();
							const MutationIndex *mut_ptr = mutrun->begin_pointer_const();
							
							for (int mut_index = 0; mut_index < mut_count; ++mut_index)
							{
								Mutation *scan_mutation = mut_block_ptr + mut_ptr[mut_index];
								
								// do a linear search for each mutation, ouch; but this is output code, so it doesn't need to be fast, probably.
								if (std::find(mutations.begin(), mutations.end(), scan_mutation) != mutations.end())
									AddMutationToPolymorphismMap(&polymorphisms, scan_mutation);
							}
						}
					}
					
					// output the frequencies of these mutations in each subpopulation; note the format here comes from the old tracked mutations code
					// NOTE the format of this output changed because print_no_id() added the mutation_id_ to its output; BCH 11 June 2016
					for (const PolymorphismPair &polymorphism_pair : polymorphisms) 
					{ 
						out << "#OUT: " << generation_ << " T p" << subpop_pair.first << " ";
						polymorphism_pair.second.print_no_id(out);
					}
				}
			}
			
			if (has_file)
				outfile.close(); 
			
			return gStaticEidosValueNULLInvisible;
		}
			
			
			//
			//	*********************	- (integer$)readFromPopulationFile(string$ filePath)
			//
#pragma mark -readFromPopulationFile()
			
		case gID_readFromPopulationFile:
		{
			if ((GenerationStage() == SLiMGenerationStage::kStage1ExecuteEarlyScripts) && (!warned_early_read_))
			{
				p_interpreter.ExecutionOutputStream() << "#WARNING (SLiMSim::ExecuteInstanceMethod): readFromPopulationFile() should probably not be called from an early() event; fitness values will not be recalculated prior to offspring generation unless recalculateFitness() is called." << std::endl;
				warned_early_read_ = true;
			}
			
			string file_path = EidosResolvedPath(arg0_value->StringAtIndex(0, nullptr));
			
			// first we clear out all variables of type Subpopulation etc. from the symbol table; they will all be invalid momentarily
			// note that we do this not only in our constants table, but in the user's variables as well; we can leave no stone unturned
			// FIXME: Note that we presently have no way of clearing out EidosScribe/SLiMgui references (the variable browser, in particular),
			// and so EidosConsoleWindowController has to do an ugly and only partly effective hack to work around this issue.
			const char *file_path_cstr = file_path.c_str();
			slim_generation_t file_generation = InitializePopulationFromFile(file_path_cstr, &p_interpreter);
			
			return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Int_singleton(file_generation));
		}
			
			
			//
			//	*********************	– (void)recalculateFitness([Ni$ generation = NULL])
			//
#pragma mark -recalculateFitness()
			
		case gID_recalculateFitness:
		{
			// Trigger a fitness recalculation.  This is suggested after making a change that would modify fitness values, such as altering
			// a selection coefficient or dominance coefficient, changing the mutation type for a mutation, etc.  It will have the side
			// effect of calling fitness() callbacks, so this is quite a heavyweight operation.
			slim_generation_t gen = (arg0_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : generation_;
			
			population_.RecalculateFitness(gen);
			
			return gStaticEidosValueNULLInvisible;
		}
			
			
			//
			//	*********************	– (object<SLiMEidosBlock>$)registerEarlyEvent(Nis$ id, string$ source, [Ni$ start = NULL], [Ni$ end = NULL])
			//	*********************	– (object<SLiMEidosBlock>$)registerLateEvent(Nis$ id, string$ source, [Ni$ start = NULL], [Ni$ end = NULL])
			//
#pragma mark -registerEarlyEvent()
#pragma mark -registerLateEvent()
			
		case gID_registerEarlyEvent:
		case gID_registerLateEvent:
		{
			slim_objectid_t script_id = -1;		// used if the arg0 is NULL, to indicate an anonymous block
			string script_string = arg1_value->StringAtIndex(0, nullptr);
			slim_generation_t start_generation = ((arg2_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg2_value->IntAtIndex(0, nullptr)) : 1);
			slim_generation_t end_generation = ((arg3_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg3_value->IntAtIndex(0, nullptr)) : SLIM_MAX_GENERATION);
			
			if (arg0_value->Type() != EidosValueType::kValueNULL)
				script_id = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 's', nullptr);
			
			if (start_generation > end_generation)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): register" << ((p_method_id == gID_registerEarlyEvent) ? "Early" : "Late") << "Event() requires start <= end." << eidos_terminate();
			
			SLiMEidosBlock *new_script_block = new SLiMEidosBlock(script_id, script_string, (p_method_id == gID_registerEarlyEvent) ? SLiMEidosBlockType::SLiMEidosEventEarly : SLiMEidosBlockType::SLiMEidosEventLate, start_generation, end_generation);
			
			AddScriptBlock(new_script_block, &p_interpreter, nullptr);		// takes ownership from us
			
			return new_script_block->SelfSymbolTableEntry().second;
		}
			
			
			//
			//	*********************	– (object<SLiMEidosBlock>$)registerFitnessCallback(Nis$ id, string$ source, Nio<MutationType>$ mutType, [Nio<Subpopulation>$ subpop = NULL], [Ni$ start = NULL], [Ni$ end = NULL])
			//
#pragma mark -registerFitnessCallback()
			
		case gID_registerFitnessCallback:
		{
			slim_objectid_t script_id = -1;		// used if arg0 is NULL, to indicate an anonymous block
			string script_string = arg1_value->StringAtIndex(0, nullptr);
			slim_objectid_t mut_type_id = -2;	// used if arg2 is NULL, to indicate a global fitness() callback
			slim_objectid_t subpop_id = -1;		// used if arg3 is NULL, to indicate applicability to all subpops
			slim_generation_t start_generation = ((arg4_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg4_value->IntAtIndex(0, nullptr)) : 1);
			slim_generation_t end_generation = ((arg5_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg5_value->IntAtIndex(0, nullptr)) : SLIM_MAX_GENERATION);
			
			if (arg0_value->Type() != EidosValueType::kValueNULL)
				script_id = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 's', nullptr);
			
			if (arg2_value->Type() != EidosValueType::kValueNULL)
				mut_type_id = (arg2_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg2_value->IntAtIndex(0, nullptr)) : ((MutationType *)arg2_value->ObjectElementAtIndex(0, nullptr))->mutation_type_id_;
			
			if (arg3_value->Type() != EidosValueType::kValueNULL)
				subpop_id = (arg3_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg3_value->IntAtIndex(0, nullptr)) : ((Subpopulation *)arg3_value->ObjectElementAtIndex(0, nullptr))->subpopulation_id_;
			
			if (start_generation > end_generation)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): registerFitnessCallback() requires start <= end." << eidos_terminate();
			
			SLiMEidosBlockType block_type = ((mut_type_id == -2) ? SLiMEidosBlockType::SLiMEidosFitnessGlobalCallback : SLiMEidosBlockType::SLiMEidosFitnessCallback);
			
			SLiMEidosBlock *new_script_block = new SLiMEidosBlock(script_id, script_string, block_type, start_generation, end_generation);
			
			new_script_block->mutation_type_id_ = mut_type_id;
			new_script_block->subpopulation_id_ = subpop_id;
			
			AddScriptBlock(new_script_block, &p_interpreter, nullptr);		// takes ownership from us
			
			return new_script_block->SelfSymbolTableEntry().second;
		}
			
			
			//
			//	*********************	– (object<SLiMEidosBlock>$)registerInteractionCallback(Nis$ id, string$ source, io<InteractionType>$ intType, [Nio<Subpopulation>$ subpop = NULL], [Ni$ start = NULL], [Ni$ end = NULL])
			//
#pragma mark -registerInteractionCallback()
			
		case gID_registerInteractionCallback:
		{
			slim_objectid_t script_id = -1;		// used if the arg0 is NULL, to indicate an anonymous block
			string script_string = arg1_value->StringAtIndex(0, nullptr);
			slim_objectid_t int_type_id = (arg2_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg2_value->IntAtIndex(0, nullptr)) : ((InteractionType *)arg2_value->ObjectElementAtIndex(0, nullptr))->interaction_type_id_;
			slim_objectid_t subpop_id = -1;
			slim_generation_t start_generation = ((arg4_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg4_value->IntAtIndex(0, nullptr)) : 1);
			slim_generation_t end_generation = ((arg5_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg5_value->IntAtIndex(0, nullptr)) : SLIM_MAX_GENERATION);
			
			if (arg0_value->Type() != EidosValueType::kValueNULL)
				script_id = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 's', nullptr);
			
			if (arg3_value->Type() != EidosValueType::kValueNULL)
				subpop_id = (arg3_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg3_value->IntAtIndex(0, nullptr)) : ((Subpopulation *)arg3_value->ObjectElementAtIndex(0, nullptr))->subpopulation_id_;
			
			if (start_generation > end_generation)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): registerInteractionCallback() requires start <= end." << eidos_terminate();
			
			SLiMEidosBlock *new_script_block = new SLiMEidosBlock(script_id, script_string, SLiMEidosBlockType::SLiMEidosInteractionCallback, start_generation, end_generation);
			
			new_script_block->interaction_type_id_ = int_type_id;
			new_script_block->subpopulation_id_ = subpop_id;
			
			AddScriptBlock(new_script_block, &p_interpreter, nullptr);		// takes ownership from us
			
			return new_script_block->SelfSymbolTableEntry().second;
		}
			
			
			//
			//	*********************	– (object<SLiMEidosBlock>$)registerMateChoiceCallback(Nis$ id, string$ source, [Nio<Subpopulation>$ subpop = NULL], [Ni$ start = NULL], [Ni$ end = NULL])
			//	*********************	– (object<SLiMEidosBlock>$)registerModifyChildCallback(Nis$ id, string$ source, [Nio<Subpopulation>$ subpop = NULL], [Ni$ start = NULL], [Ni$ end = NULL])
			//	*********************	– (object<SLiMEidosBlock>$)registerRecombinationCallback(Nis$ id, string$ source, [Nio<Subpopulation>$ subpop = NULL], [Ni$ start = NULL], [Ni$ end = NULL])
			//
#pragma mark -registerMateChoiceCallback()
#pragma mark -registerModifyChildCallback()
#pragma mark -registerRecombinationCallback()
			
		case gID_registerMateChoiceCallback:
		case gID_registerModifyChildCallback:
		case gID_registerRecombinationCallback:
		{
			slim_objectid_t script_id = -1;		// used if the arg0 is NULL, to indicate an anonymous block
			string script_string = arg1_value->StringAtIndex(0, nullptr);
			slim_objectid_t subpop_id = -1;
			slim_generation_t start_generation = ((arg3_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg3_value->IntAtIndex(0, nullptr)) : 1);
			slim_generation_t end_generation = ((arg4_value->Type() != EidosValueType::kValueNULL) ? SLiMCastToGenerationTypeOrRaise(arg4_value->IntAtIndex(0, nullptr)) : SLIM_MAX_GENERATION);
			
			if (arg0_value->Type() != EidosValueType::kValueNULL)
				script_id = (arg0_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg0_value->IntAtIndex(0, nullptr)) : SLiMEidosScript::ExtractIDFromStringWithPrefix(arg0_value->StringAtIndex(0, nullptr), 's', nullptr);
			
			if (arg2_value->Type() != EidosValueType::kValueNULL)
				subpop_id = (arg2_value->Type() == EidosValueType::kValueInt) ? SLiMCastToObjectidTypeOrRaise(arg2_value->IntAtIndex(0, nullptr)) : ((Subpopulation *)arg2_value->ObjectElementAtIndex(0, nullptr))->subpopulation_id_;
			
			if (start_generation > end_generation)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): " << StringForEidosGlobalStringID(p_method_id) << " requires start <= end." << eidos_terminate();
			
			SLiMEidosBlockType block_type;
			
			if (p_method_id == gID_registerMateChoiceCallback)				block_type = SLiMEidosBlockType::SLiMEidosMateChoiceCallback;
			else if (p_method_id == gID_registerModifyChildCallback)		block_type = SLiMEidosBlockType::SLiMEidosModifyChildCallback;
			else /* (p_method_id == gID_registerRecombinationCallback) */	block_type = SLiMEidosBlockType::SLiMEidosRecombinationCallback;
			
			SLiMEidosBlock *new_script_block = new SLiMEidosBlock(script_id, script_string, block_type, start_generation, end_generation);
			
			new_script_block->subpopulation_id_ = subpop_id;
			
			AddScriptBlock(new_script_block, &p_interpreter, nullptr);		// takes ownership from us
			
			return new_script_block->SelfSymbolTableEntry().second;
		}
			
			
			//
			//	*********************	– (object<SLiMEidosBlock>)rescheduleScriptBlock(object<SLiMEidosBlock>$ block, [Ni$ start = NULL], [Ni$ end = NULL], [Ni generations = NULL])
			//
#pragma mark -rescheduleScriptBlock()
			
		case gID_rescheduleScriptBlock:
		{
			EidosValue_Object *block_value = (EidosValue_Object *)arg0_value;
			SLiMEidosBlock *block = (SLiMEidosBlock *)block_value->ObjectElementAtIndex(0, nullptr);
			EidosValue *start_value = arg1_value;
			EidosValue *end_value = arg2_value;
			EidosValue *generations_value = arg3_value;
			bool start_null = (start_value->Type() == EidosValueType::kValueNULL);
			bool end_null = (end_value->Type() == EidosValueType::kValueNULL);
			bool generations_null = (generations_value->Type() == EidosValueType::kValueNULL);
			
			if (block->type_ == SLiMEidosBlockType::SLiMEidosUserDefinedFunction)
			{
				// this should never be hit, because the user should have to way to get a reference to a function block
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): (internal error) rescheduleScriptBlock() cannot be called on user-defined function script blocks." << eidos_terminate();
			}
			
			SLiMSim *sim = dynamic_cast<SLiMSim *>(p_interpreter.Context());
			
			if (!sim)
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): (internal error) the sim is not registered as the context pointer." << eidos_terminate();
			
			if ((!start_null || !end_null) && generations_null)
			{
				// start/end case; this is simple
				
				slim_generation_t start = (start_null ? 1 : SLiMCastToGenerationTypeOrRaise(start_value->IntAtIndex(0, nullptr)));
				slim_generation_t end = (end_null ? SLIM_MAX_GENERATION : SLiMCastToGenerationTypeOrRaise(end_value->IntAtIndex(0, nullptr)));
				
				if (start > end)
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): reschedule() requires start <= end." << eidos_terminate();
				
				block->start_generation_ = start;
				block->end_generation_ = end;
				last_script_block_gen_cached = false;
				script_block_types_cached_ = false;
				scripts_changed_ = true;
				
				return EidosValue_SP(new (gEidosValuePool->AllocateChunk()) EidosValue_Object_singleton(block, gSLiM_SLiMEidosBlock_Class));
			}
			else if (!generations_null && (start_null && end_null))
			{
				// generations case; this is complicated
				
				// first, fetch the generations and make sure they are in bounds
				std::vector<slim_generation_t> generations;
				int gen_count = generations_value->Count();
				
				if (gen_count < 1)
					EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): reschedule() requires at least one generation; use deregisterScriptBlock() to remove a script block from the simulation." << eidos_terminate();
				
				generations.reserve(gen_count);
				
				for (int gen_index = 0; gen_index < gen_count; ++gen_index)
					generations.push_back(SLiMCastToGenerationTypeOrRaise(generations_value->IntAtIndex(gen_index, nullptr)));
				
				// next, sort the generation list
				std::sort(generations.begin(), generations.end());
				
				// finally, go through the generation vector and schedule blocks for sequential runs
				EidosValue_Object_vector *vec = (new (gEidosValuePool->AllocateChunk()) EidosValue_Object_vector(gSLiM_SLiMEidosBlock_Class));
				EidosValue_SP result_SP = EidosValue_SP(vec);
				bool first_block = true;
				
				slim_generation_t start = -10;
				slim_generation_t end = -10;
				int gen_index = 0;
				
				// I'm sure there's a prettier algorithm for finding the sequential runs, but I'm not seeing it right now.
				// The tricky thing is that I want there to be only a single place in the code where a block is scheduled;
				// it seems easy to write a version where blocks get scheduled in two places, a main case and a tail case.
				while (true)
				{
					slim_generation_t gen = generations[gen_index];
					bool reached_end_in_seq = false;
					
					if (gen == end + 1)			// sequential value seen; move on to the next sequential value
					{
						end++;
						
						if (++gen_index < gen_count)
							continue;
						reached_end_in_seq = true;
					}
					else if (gen <= end)
					{
						EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): reschedule() requires that the generation vector contain unique values; the same generation cannot be used twice." << eidos_terminate();
					}
					
					// make new block and move on to start the next sequence
				makeBlock:
					if ((start != -10) && (end != -10))
					{
						// start and end define the range of the block to schedule; first_block
						// determines whether we use the existing block or make a new one
						if (first_block)
						{
							block->start_generation_ = start;
							block->end_generation_ = end;
							first_block = false;
							last_script_block_gen_cached = false;
							script_block_types_cached_ = false;
							scripts_changed_ = true;
							
							vec->PushObjectElement(block);
						}
						else
						{
							SLiMEidosBlock *new_script_block = new SLiMEidosBlock(-1, block->compound_statement_node_->token_->token_string_, block->type_, start, end);
							
							AddScriptBlock(new_script_block, &p_interpreter, nullptr);		// takes ownership from us
							
							vec->PushObjectElement(new_script_block);
						}
					}
					
					start = gen;
					end = gen;
					++gen_index;
					
					if ((gen_index == gen_count) && !reached_end_in_seq)
						goto makeBlock;
					else if (gen_index >= gen_count)
						break;
				}
				
				return result_SP;
			}
			else
			{
				EIDOS_TERMINATION << "ERROR (SLiMSim::ExecuteInstanceMethod): reschedule() requires that either start/end or generations be supplied, but not both." << eidos_terminate();
			}
		}
			
			
			//
			//	*********************	- (void)simulationFinished(void)
			//
#pragma mark -simulationFinished()
			
		case gID_simulationFinished:
		{
			sim_declared_finished_ = true;
			
			return gStaticEidosValueNULLInvisible;
		}
			
			// all others, including gID_none
		default:
			return SLiMEidosDictionary::ExecuteInstanceMethod(p_method_id, p_arguments, p_argument_count, p_interpreter);
	}
}


//
//	SLiMSim_Class
//
#pragma mark -
#pragma mark SLiMSim_Class

class SLiMSim_Class : public SLiMEidosDictionary_Class
{
public:
	SLiMSim_Class(const SLiMSim_Class &p_original) = delete;	// no copy-construct
	SLiMSim_Class& operator=(const SLiMSim_Class&) = delete;	// no copying
	
	SLiMSim_Class(void);
	
	virtual const std::string &ElementType(void) const;
	
	virtual const std::vector<const EidosPropertySignature *> *Properties(void) const;
	virtual const EidosPropertySignature *SignatureForProperty(EidosGlobalStringID p_property_id) const;
	
	virtual const std::vector<const EidosMethodSignature *> *Methods(void) const;
	virtual const EidosMethodSignature *SignatureForMethod(EidosGlobalStringID p_method_id) const;
	virtual EidosValue_SP ExecuteClassMethod(EidosGlobalStringID p_method_id, EidosValue_Object *p_target, const EidosValue_SP *const p_arguments, int p_argument_count, EidosInterpreter &p_interpreter) const;
};

EidosObjectClass *gSLiM_SLiMSim_Class = new SLiMSim_Class();


SLiMSim_Class::SLiMSim_Class(void)
{
}

const std::string &SLiMSim_Class::ElementType(void) const
{
	return gStr_SLiMSim;
}

const std::vector<const EidosPropertySignature *> *SLiMSim_Class::Properties(void) const
{
	static std::vector<const EidosPropertySignature *> *properties = nullptr;
	
	if (!properties)
	{
		properties = new std::vector<const EidosPropertySignature *>(*EidosObjectClass::Properties());
		properties->emplace_back(SignatureForPropertyOrRaise(gID_chromosome));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_chromosomeType));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_dimensionality));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_genomicElementTypes));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_inSLiMgui));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_interactionTypes));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_mutations));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_mutationTypes));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_scriptBlocks));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_sexEnabled));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_subpopulations));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_substitutions));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_dominanceCoeffX));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_generation));
		properties->emplace_back(SignatureForPropertyOrRaise(gID_tag));
		std::sort(properties->begin(), properties->end(), CompareEidosPropertySignatures);
	}
	
	return properties;
}

const EidosPropertySignature *SLiMSim_Class::SignatureForProperty(EidosGlobalStringID p_property_id) const
{
	// Signatures are all preallocated, for speed
	static EidosPropertySignature *chromosomeSig = nullptr;
	static EidosPropertySignature *chromosomeTypeSig = nullptr;
	static EidosPropertySignature *dimensionalitySig = nullptr;
	static EidosPropertySignature *genomicElementTypesSig = nullptr;
	static EidosPropertySignature *inSLiMguiSig = nullptr;
	static EidosPropertySignature *interactionTypesSig = nullptr;
	static EidosPropertySignature *mutationsSig = nullptr;
	static EidosPropertySignature *mutationTypesSig = nullptr;
	static EidosPropertySignature *scriptBlocksSig = nullptr;
	static EidosPropertySignature *sexEnabledSig = nullptr;
	static EidosPropertySignature *subpopulationsSig = nullptr;
	static EidosPropertySignature *substitutionsSig = nullptr;
	static EidosPropertySignature *dominanceCoeffXSig = nullptr;
	static EidosPropertySignature *generationSig = nullptr;
	static EidosPropertySignature *tagSig = nullptr;
	
	if (!chromosomeSig)
	{
		chromosomeSig =				(EidosPropertySignature *)(new EidosPropertySignature(gStr_chromosome,			gID_chromosome,				true,	kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_Chromosome_Class));
		chromosomeTypeSig =			(EidosPropertySignature *)(new EidosPropertySignature(gStr_chromosomeType,		gID_chromosomeType,			true,	kEidosValueMaskString | kEidosValueMaskSingleton));
		dimensionalitySig =			(EidosPropertySignature *)(new EidosPropertySignature(gStr_dimensionality,		gID_dimensionality,			true,	kEidosValueMaskString | kEidosValueMaskSingleton));
		genomicElementTypesSig =	(EidosPropertySignature *)(new EidosPropertySignature(gStr_genomicElementTypes,	gID_genomicElementTypes,	true,	kEidosValueMaskObject, gSLiM_GenomicElementType_Class));
		inSLiMguiSig =				(EidosPropertySignature *)(new EidosPropertySignature(gStr_inSLiMgui,			gID_inSLiMgui,				true,	kEidosValueMaskLogical | kEidosValueMaskSingleton));
		interactionTypesSig =		(EidosPropertySignature *)(new EidosPropertySignature(gStr_interactionTypes,	gID_interactionTypes,		true,	kEidosValueMaskObject, gSLiM_InteractionType_Class));
		mutationsSig =				(EidosPropertySignature *)(new EidosPropertySignature(gStr_mutations,			gID_mutations,				true,	kEidosValueMaskObject, gSLiM_Mutation_Class));
		mutationTypesSig =			(EidosPropertySignature *)(new EidosPropertySignature(gStr_mutationTypes,		gID_mutationTypes,			true,	kEidosValueMaskObject, gSLiM_MutationType_Class));
		scriptBlocksSig =			(EidosPropertySignature *)(new EidosPropertySignature(gStr_scriptBlocks,		gID_scriptBlocks,			true,	kEidosValueMaskObject, gSLiM_SLiMEidosBlock_Class));
		sexEnabledSig =				(EidosPropertySignature *)(new EidosPropertySignature(gStr_sexEnabled,			gID_sexEnabled,				true,	kEidosValueMaskLogical | kEidosValueMaskSingleton));
		subpopulationsSig =			(EidosPropertySignature *)(new EidosPropertySignature(gStr_subpopulations,		gID_subpopulations,			true,	kEidosValueMaskObject, gSLiM_Subpopulation_Class));
		substitutionsSig =			(EidosPropertySignature *)(new EidosPropertySignature(gStr_substitutions,		gID_substitutions,			true,	kEidosValueMaskObject, gSLiM_Substitution_Class));
		dominanceCoeffXSig =		(EidosPropertySignature *)(new EidosPropertySignature(gStr_dominanceCoeffX,		gID_dominanceCoeffX,		false,	kEidosValueMaskFloat | kEidosValueMaskSingleton));
		generationSig =				(EidosPropertySignature *)(new EidosPropertySignature(gStr_generation,			gID_generation,				false,	kEidosValueMaskInt | kEidosValueMaskSingleton));
		tagSig =					(EidosPropertySignature *)(new EidosPropertySignature(gStr_tag,					gID_tag,					false,	kEidosValueMaskInt | kEidosValueMaskSingleton));
	}
	
	// All of our strings are in the global registry, so we can require a successful lookup
	switch (p_property_id)
	{
		case gID_chromosome:			return chromosomeSig;
		case gID_chromosomeType:		return chromosomeTypeSig;
		case gID_dimensionality:		return dimensionalitySig;
		case gID_genomicElementTypes:	return genomicElementTypesSig;
		case gID_inSLiMgui:				return inSLiMguiSig;
		case gID_interactionTypes:		return interactionTypesSig;
		case gID_mutations:				return mutationsSig;
		case gID_mutationTypes:			return mutationTypesSig;
		case gID_scriptBlocks:			return scriptBlocksSig;
		case gID_sexEnabled:			return sexEnabledSig;
		case gID_subpopulations:		return subpopulationsSig;
		case gID_substitutions:			return substitutionsSig;
		case gID_dominanceCoeffX:		return dominanceCoeffXSig;
		case gID_generation:			return generationSig;
		case gID_tag:					return tagSig;
			
			// all others, including gID_none
		default:
			return EidosObjectClass::SignatureForProperty(p_property_id);
	}
}

const std::vector<const EidosMethodSignature *> *SLiMSim_Class::Methods(void) const
{
	static std::vector<const EidosMethodSignature *> *methods = nullptr;
	
	if (!methods)
	{
		methods = new std::vector<const EidosMethodSignature *>(*SLiMEidosDictionary_Class::Methods());
		methods->emplace_back(SignatureForMethodOrRaise(gID_addSubpop));
		methods->emplace_back(SignatureForMethodOrRaise(gID_addSubpopSplit));
		methods->emplace_back(SignatureForMethodOrRaise(gID_deregisterScriptBlock));
		methods->emplace_back(SignatureForMethodOrRaise(gID_mutationFrequencies));
		methods->emplace_back(SignatureForMethodOrRaise(gID_mutationCounts));
		methods->emplace_back(SignatureForMethodOrRaise(gID_mutationsOfType));
		methods->emplace_back(SignatureForMethodOrRaise(gID_countOfMutationsOfType));
		methods->emplace_back(SignatureForMethodOrRaise(gID_outputFixedMutations));
		methods->emplace_back(SignatureForMethodOrRaise(gID_outputFull));
		methods->emplace_back(SignatureForMethodOrRaise(gID_outputMutations));
		methods->emplace_back(SignatureForMethodOrRaise(gID_readFromPopulationFile));
		methods->emplace_back(SignatureForMethodOrRaise(gID_recalculateFitness));
		methods->emplace_back(SignatureForMethodOrRaise(gID_registerEarlyEvent));
		methods->emplace_back(SignatureForMethodOrRaise(gID_registerLateEvent));
		methods->emplace_back(SignatureForMethodOrRaise(gID_registerFitnessCallback));
		methods->emplace_back(SignatureForMethodOrRaise(gID_registerInteractionCallback));
		methods->emplace_back(SignatureForMethodOrRaise(gID_registerMateChoiceCallback));
		methods->emplace_back(SignatureForMethodOrRaise(gID_registerModifyChildCallback));
		methods->emplace_back(SignatureForMethodOrRaise(gID_registerRecombinationCallback));
		methods->emplace_back(SignatureForMethodOrRaise(gID_rescheduleScriptBlock));
		methods->emplace_back(SignatureForMethodOrRaise(gID_simulationFinished));
		std::sort(methods->begin(), methods->end(), CompareEidosCallSignatures);
	}
	
	return methods;
}

const EidosMethodSignature *SLiMSim_Class::SignatureForMethod(EidosGlobalStringID p_method_id) const
{
	// Signatures are all preallocated, for speed
	static EidosInstanceMethodSignature *addSubpopSig = nullptr;
	static EidosInstanceMethodSignature *addSubpopSplitSig = nullptr;
	static EidosInstanceMethodSignature *deregisterScriptBlockSig = nullptr;
	static EidosInstanceMethodSignature *mutationFrequenciesSig = nullptr;
	static EidosInstanceMethodSignature *mutationCountsSig = nullptr;
	static EidosInstanceMethodSignature *mutationsOfTypeSig = nullptr;
	static EidosInstanceMethodSignature *countOfMutationsOfTypeSig = nullptr;
	static EidosInstanceMethodSignature *outputFixedMutationsSig = nullptr;
	static EidosInstanceMethodSignature *outputFullSig = nullptr;
	static EidosInstanceMethodSignature *outputMutationsSig = nullptr;
	static EidosInstanceMethodSignature *readFromPopulationFileSig = nullptr;
	static EidosInstanceMethodSignature *recalculateFitnessSig = nullptr;
	static EidosInstanceMethodSignature *registerEarlyEventSig = nullptr;
	static EidosInstanceMethodSignature *registerLateEventSig = nullptr;
	static EidosInstanceMethodSignature *registerFitnessCallbackSig = nullptr;
	static EidosInstanceMethodSignature *registerInteractionCallbackSig = nullptr;
	static EidosInstanceMethodSignature *registerMateChoiceCallbackSig = nullptr;
	static EidosInstanceMethodSignature *registerModifyChildCallbackSig = nullptr;
	static EidosInstanceMethodSignature *registerRecombinationCallbackSig = nullptr;
	static EidosInstanceMethodSignature *rescheduleScriptBlockSig = nullptr;
	static EidosInstanceMethodSignature *simulationFinishedSig = nullptr;
	
	if (!addSubpopSig)
	{
		addSubpopSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_addSubpop, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_Subpopulation_Class))->AddIntString_S("subpopID")->AddInt_S("size")->AddFloat_OS("sexRatio", gStaticEidosValue_Float0Point5);
		addSubpopSplitSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_addSubpopSplit, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_Subpopulation_Class))->AddIntString_S("subpopID")->AddInt_S("size")->AddIntObject_S("sourceSubpop", gSLiM_Subpopulation_Class)->AddFloat_OS("sexRatio", gStaticEidosValue_Float0Point5);
		countOfMutationsOfTypeSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_countOfMutationsOfType, kEidosValueMaskInt | kEidosValueMaskSingleton))->AddIntObject_S("mutType", gSLiM_MutationType_Class);
		deregisterScriptBlockSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_deregisterScriptBlock, kEidosValueMaskNULL))->AddIntObject("scriptBlocks", gSLiM_SLiMEidosBlock_Class);
		mutationFrequenciesSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_mutationFrequencies, kEidosValueMaskFloat))->AddObject_N("subpops", gSLiM_Subpopulation_Class)->AddObject_ON("mutations", gSLiM_Mutation_Class, gStaticEidosValueNULL);
		mutationCountsSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_mutationCounts, kEidosValueMaskInt))->AddObject_N("subpops", gSLiM_Subpopulation_Class)->AddObject_ON("mutations", gSLiM_Mutation_Class, gStaticEidosValueNULL);
		mutationsOfTypeSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_mutationsOfType, kEidosValueMaskObject, gSLiM_Mutation_Class))->AddIntObject_S("mutType", gSLiM_MutationType_Class);
		outputFixedMutationsSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_outputFixedMutations, kEidosValueMaskNULL))->AddString_OSN("filePath", gStaticEidosValueNULL)->AddLogical_OS("append", gStaticEidosValue_LogicalF);
		outputFullSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_outputFull, kEidosValueMaskNULL))->AddString_OSN("filePath", gStaticEidosValueNULL)->AddLogical_OS("binary", gStaticEidosValue_LogicalF)->AddLogical_OS("append", gStaticEidosValue_LogicalF)->AddLogical_OS("spatialPositions", gStaticEidosValue_LogicalT);
		outputMutationsSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_outputMutations, kEidosValueMaskNULL))->AddObject("mutations", gSLiM_Mutation_Class)->AddString_OSN("filePath", gStaticEidosValueNULL)->AddLogical_OS("append", gStaticEidosValue_LogicalF);
		readFromPopulationFileSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_readFromPopulationFile, kEidosValueMaskInt | kEidosValueMaskSingleton))->AddString_S("filePath");
		recalculateFitnessSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_recalculateFitness, kEidosValueMaskNULL))->AddInt_OSN("generation", gStaticEidosValueNULL);
		registerEarlyEventSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_registerEarlyEvent, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_SLiMEidosBlock_Class))->AddIntString_SN("id")->AddString_S("source")->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL);
		registerLateEventSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_registerLateEvent, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_SLiMEidosBlock_Class))->AddIntString_SN("id")->AddString_S("source")->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL);
		registerFitnessCallbackSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_registerFitnessCallback, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_SLiMEidosBlock_Class))->AddIntString_SN("id")->AddString_S("source")->AddIntObject_SN("mutType", gSLiM_MutationType_Class)->AddIntObject_OSN("subpop", gSLiM_Subpopulation_Class, gStaticEidosValueNULL)->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL);
		registerInteractionCallbackSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_registerInteractionCallback, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_SLiMEidosBlock_Class))->AddIntString_SN("id")->AddString_S("source")->AddIntObject_S("intType", gSLiM_InteractionType_Class)->AddIntObject_OSN("subpop", gSLiM_Subpopulation_Class, gStaticEidosValueNULL)->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL);
		registerMateChoiceCallbackSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_registerMateChoiceCallback, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_SLiMEidosBlock_Class))->AddIntString_SN("id")->AddString_S("source")->AddIntObject_OSN("subpop", gSLiM_Subpopulation_Class, gStaticEidosValueNULL)->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL);
		registerModifyChildCallbackSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_registerModifyChildCallback, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_SLiMEidosBlock_Class))->AddIntString_SN("id")->AddString_S("source")->AddIntObject_OSN("subpop", gSLiM_Subpopulation_Class, gStaticEidosValueNULL)->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL);
		registerRecombinationCallbackSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_registerRecombinationCallback, kEidosValueMaskObject | kEidosValueMaskSingleton, gSLiM_SLiMEidosBlock_Class))->AddIntString_SN("id")->AddString_S("source")->AddIntObject_OSN("subpop", gSLiM_Subpopulation_Class, gStaticEidosValueNULL)->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL);
		rescheduleScriptBlockSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_rescheduleScriptBlock, kEidosValueMaskObject, gSLiM_SLiMEidosBlock_Class))->AddObject_S("block", gSLiM_SLiMEidosBlock_Class)->AddInt_OSN("start", gStaticEidosValueNULL)->AddInt_OSN("end", gStaticEidosValueNULL)->AddInt_ON("generations", gStaticEidosValueNULL);
		simulationFinishedSig = (EidosInstanceMethodSignature *)(new EidosInstanceMethodSignature(gStr_simulationFinished, kEidosValueMaskNULL));
	}
	
	// All of our strings are in the global registry, so we can require a successful lookup
	switch (p_method_id)
	{
		case gID_addSubpop:								return addSubpopSig;
		case gID_addSubpopSplit:						return addSubpopSplitSig;
		case gID_countOfMutationsOfType:				return countOfMutationsOfTypeSig;
		case gID_deregisterScriptBlock:					return deregisterScriptBlockSig;
		case gID_mutationFrequencies:					return mutationFrequenciesSig;
		case gID_mutationCounts:						return mutationCountsSig;
		case gID_mutationsOfType:						return mutationsOfTypeSig;
		case gID_outputFixedMutations:					return outputFixedMutationsSig;
		case gID_outputFull:							return outputFullSig;
		case gID_outputMutations:						return outputMutationsSig;
		case gID_readFromPopulationFile:				return readFromPopulationFileSig;
		case gID_recalculateFitness:					return recalculateFitnessSig;
		case gID_registerEarlyEvent:					return registerEarlyEventSig;
		case gID_registerLateEvent:						return registerLateEventSig;
		case gID_registerFitnessCallback:				return registerFitnessCallbackSig;
		case gID_registerInteractionCallback:			return registerInteractionCallbackSig;
		case gID_registerMateChoiceCallback:			return registerMateChoiceCallbackSig;
		case gID_registerModifyChildCallback:			return registerModifyChildCallbackSig;
		case gID_registerRecombinationCallback:			return registerRecombinationCallbackSig;
		case gID_rescheduleScriptBlock:					return rescheduleScriptBlockSig;
		case gID_simulationFinished:					return simulationFinishedSig;
			
			// all others, including gID_none
		default:
			return SLiMEidosDictionary_Class::SignatureForMethod(p_method_id);
	}
}

EidosValue_SP SLiMSim_Class::ExecuteClassMethod(EidosGlobalStringID p_method_id, EidosValue_Object *p_target, const EidosValue_SP *const p_arguments, int p_argument_count, EidosInterpreter &p_interpreter) const
{
	return EidosObjectClass::ExecuteClassMethod(p_method_id, p_target, p_arguments, p_argument_count, p_interpreter);
}




























































