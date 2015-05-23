//
//  script_functionsignature.cpp
//  SLiM
//
//  Created by Ben Haller on 5/16/15.
//  Copyright (c) 2015 Philipp Messer.  All rights reserved.
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


#include "script_functionsignature.h"


using std::string;
using std::vector;
using std::endl;
using std::ostream;


//
//	FunctionSignature
//
#pragma mark FunctionSignature

FunctionSignature::FunctionSignature(string p_function_name, FunctionIdentifier p_function_id, ScriptValueMask p_return_mask) :
function_name_(p_function_name), function_id_(p_function_id), return_mask_(p_return_mask)
{
}

FunctionSignature::FunctionSignature(string p_function_name, FunctionIdentifier p_function_id, ScriptValueMask p_return_mask, SLiMDelegateFunctionPtr p_delegate_function, void *p_delegate_object, string p_delegate_name) :
function_name_(p_function_name), function_id_(p_function_id), return_mask_(p_return_mask), delegate_function_(p_delegate_function), delegate_object_(p_delegate_object), delegate_name_(p_delegate_name)
{
}

FunctionSignature *FunctionSignature::SetClassMethod()
{
	is_class_method = true;
	return this;
}

FunctionSignature *FunctionSignature::SetInstanceMethod()
{
	is_instance_method = true;
	return this;
}

FunctionSignature *FunctionSignature::AddArg(ScriptValueMask p_arg_mask)
{
	bool is_optional = !!(p_arg_mask & kScriptValueMaskOptional);
	
	if (has_optional_args_ && !is_optional)
		SLIM_TERMINATION << "ERROR (FunctionSignature::AddArg): cannot add a required argument after an optional argument has been added." << endl << slim_terminate();
	
	if (has_ellipsis_)
		SLIM_TERMINATION << "ERROR (FunctionSignature::AddArg): cannot add an argument after an ellipsis." << endl << slim_terminate();
	
	arg_masks_.push_back(p_arg_mask);
	
	if (is_optional)
		has_optional_args_ = true;
	
	return this;
}

FunctionSignature *FunctionSignature::AddEllipsis()
{
	if (has_optional_args_)
		SLIM_TERMINATION << "ERROR (FunctionSignature::AddEllipsis): cannot add an ellipsis after an optional argument has been added." << endl << slim_terminate();
	
	if (has_ellipsis_)
		SLIM_TERMINATION << "ERROR (FunctionSignature::AddEllipsis): cannot add more than one ellipsis." << endl << slim_terminate();
	
	has_ellipsis_ = true;
	
	return this;
}

FunctionSignature *FunctionSignature::AddLogical()			{ return AddArg(kScriptValueMaskLogical); }
FunctionSignature *FunctionSignature::AddInt()				{ return AddArg(kScriptValueMaskInt); }
FunctionSignature *FunctionSignature::AddFloat()			{ return AddArg(kScriptValueMaskFloat); }
FunctionSignature *FunctionSignature::AddString()			{ return AddArg(kScriptValueMaskString); }
FunctionSignature *FunctionSignature::AddObject()			{ return AddArg(kScriptValueMaskObject); }
FunctionSignature *FunctionSignature::AddNumeric()			{ return AddArg(kScriptValueMaskNumeric); }
FunctionSignature *FunctionSignature::AddLogicalEquiv()		{ return AddArg(kScriptValueMaskLogicalEquiv); }
FunctionSignature *FunctionSignature::AddAnyBase()			{ return AddArg(kScriptValueMaskAnyBase); }
FunctionSignature *FunctionSignature::AddAny()				{ return AddArg(kScriptValueMaskAny); }

FunctionSignature *FunctionSignature::AddLogical_O()		{ return AddArg(kScriptValueMaskLogical | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddInt_O()			{ return AddArg(kScriptValueMaskInt | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddFloat_O()			{ return AddArg(kScriptValueMaskFloat | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddString_O()			{ return AddArg(kScriptValueMaskString | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddObject_O()			{ return AddArg(kScriptValueMaskObject | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddNumeric_O()		{ return AddArg(kScriptValueMaskNumeric | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddLogicalEquiv_O()	{ return AddArg(kScriptValueMaskLogicalEquiv | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddAnyBase_O()		{ return AddArg(kScriptValueMaskAnyBase | kScriptValueMaskOptional); }
FunctionSignature *FunctionSignature::AddAny_O()			{ return AddArg(kScriptValueMaskAny | kScriptValueMaskOptional); }

FunctionSignature *FunctionSignature::AddLogical_S()		{ return AddArg(kScriptValueMaskLogical | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddInt_S()			{ return AddArg(kScriptValueMaskInt | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddFloat_S()			{ return AddArg(kScriptValueMaskFloat | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddString_S()			{ return AddArg(kScriptValueMaskString | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddObject_S()			{ return AddArg(kScriptValueMaskObject | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddNumeric_S()		{ return AddArg(kScriptValueMaskNumeric | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddLogicalEquiv_S()	{ return AddArg(kScriptValueMaskLogicalEquiv | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddAnyBase_S()		{ return AddArg(kScriptValueMaskAnyBase | kScriptValueMaskSingleton); }
FunctionSignature *FunctionSignature::AddAny_S()			{ return AddArg(kScriptValueMaskAny | kScriptValueMaskSingleton); }

FunctionSignature *FunctionSignature::AddLogical_OS()		{ return AddArg(kScriptValueMaskLogical | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddInt_OS()			{ return AddArg(kScriptValueMaskInt | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddFloat_OS()			{ return AddArg(kScriptValueMaskFloat | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddString_OS()		{ return AddArg(kScriptValueMaskString | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddObject_OS()			{ return AddArg(kScriptValueMaskObject | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddNumeric_OS()		{ return AddArg(kScriptValueMaskNumeric | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddLogicalEquiv_OS()	{ return AddArg(kScriptValueMaskLogicalEquiv | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddAnyBase_OS()		{ return AddArg(kScriptValueMaskAnyBase | kScriptValueMaskOptSingleton); }
FunctionSignature *FunctionSignature::AddAny_OS()			{ return AddArg(kScriptValueMaskAny | kScriptValueMaskOptSingleton); }

void FunctionSignature::CheckArguments(string const &p_call_type, vector<ScriptValue*> const &p_arguments) const
{
	// Check the number of arguments supplied
	int n_args = (int)p_arguments.size();
	
	if (!has_ellipsis_)
	{
		if (n_args > arg_masks_.size())
			SLIM_TERMINATION << "ERROR (FunctionSignature::CheckArguments): " << p_call_type << " " << function_name_ << "() requires at most " << arg_masks_.size() << " argument(s), but " << n_args << " are supplied." << endl << slim_terminate();
	}
	
	// Check the types of all arguments specified in the signature
	for (int arg_index = 0; arg_index < arg_masks_.size(); ++arg_index)
	{
		ScriptValueMask type_mask = arg_masks_[arg_index];		// the static_casts are annoying but I like scoped enums
		bool is_optional = !!(type_mask & kScriptValueMaskOptional);
		bool requires_singleton = !!(type_mask & kScriptValueMaskSingleton);
		
		type_mask &= kScriptValueMaskFlagStrip;
		
		// if no argument was passed for this slot, it needs to be an optional slot
		if (n_args <= arg_index)
		{
			if (is_optional)
				break;			// all the rest of the arguments must be optional, so we're done checking
			else
				SLIM_TERMINATION << "ERROR (FunctionSignature::CheckArguments): missing required argument for " << p_call_type << " " << function_name_ << "()." << endl << slim_terminate();
		}
		
		// an argument was passed, so check its type
		ScriptValue *argument = p_arguments[arg_index];
		ScriptValueType arg_type = argument->Type();
		
		if (type_mask != kScriptValueMaskAny)
		{
			bool type_ok = true;
			
			switch (arg_type)
			{
				case ScriptValueType::kValueNULL:		type_ok = !!(type_mask & kScriptValueMaskNULL);		break;
				case ScriptValueType::kValueLogical:	type_ok = !!(type_mask & kScriptValueMaskLogical);	break;
				case ScriptValueType::kValueString:		type_ok = !!(type_mask & kScriptValueMaskString);	break;
				case ScriptValueType::kValueInt:		type_ok = !!(type_mask & kScriptValueMaskInt);		break;
				case ScriptValueType::kValueFloat:		type_ok = !!(type_mask & kScriptValueMaskFloat);	break;
				case ScriptValueType::kValueObject:		type_ok = !!(type_mask & kScriptValueMaskObject);	break;
			}
			
			if (!type_ok)
				SLIM_TERMINATION << "ERROR (FunctionSignature::CheckArguments): argument " << arg_index + 1 << " cannot be type " << arg_type << " for " << p_call_type << " " << function_name_ << "()." << endl << slim_terminate();
			
			if (requires_singleton && (argument->Count() != 1))
				SLIM_TERMINATION << "ERROR (FunctionSignature::CheckArguments): argument " << arg_index + 1 << " must be a singleton (size() == 1) for " << p_call_type << " " << function_name_ << "(), but size() == " << argument->Count() << "." << endl << slim_terminate();
		}
	}
}

void FunctionSignature::CheckReturn(string const &p_call_type, ScriptValue *p_result) const
{
	uint32_t retmask = return_mask_;
	bool return_type_ok = true;
	
	switch (p_result->Type())
	{
		case ScriptValueType::kValueNULL:
			// A return type of NULL is always allowed, in fact; we don't want to have to specify this in the return type
			// This is a little fishy, but since NULL is used to indicate error conditions, NULL returns are exceptional,
			// and the return type indicates the type ordinarily returned in non-exceptional cases.  We just return here,
			// since we also don't want to do the singleton check below (since it would raise too).
			return;
		case ScriptValueType::kValueLogical:	return_type_ok = !!(retmask & kScriptValueMaskLogical);	break;
		case ScriptValueType::kValueInt:		return_type_ok = !!(retmask & kScriptValueMaskInt);		break;
		case ScriptValueType::kValueFloat:		return_type_ok = !!(retmask & kScriptValueMaskFloat);		break;
		case ScriptValueType::kValueString:		return_type_ok = !!(retmask & kScriptValueMaskString);	break;
		case ScriptValueType::kValueObject:		return_type_ok = !!(retmask & kScriptValueMaskObject);		break;
	}
	
	if (!return_type_ok)
		SLIM_TERMINATION << "ERROR (FunctionSignature::CheckReturn): internal error: return value cannot be type " << p_result->Type() << " for " << p_call_type << " " << function_name_ << "()." << endl << slim_terminate();
	
	bool return_is_singleton = !!(retmask & kScriptValueMaskSingleton);
	
	if (return_is_singleton && (p_result->Count() != 1))
		SLIM_TERMINATION << "ERROR (FunctionSignature::CheckReturn): internal error: return value must be a singleton (size() == 1) for " << p_call_type << " " << function_name_ << "(), but size() == " << p_result->Count() << endl << slim_terminate();
}

ostream &operator<<(ostream &p_outstream, const FunctionSignature &p_signature)
{
	if (p_signature.is_class_method)
		p_outstream << "+ ";
	else if (p_signature.is_instance_method)
		p_outstream << "- ";
	
	p_outstream << "(" << StringForScriptValueMask(p_signature.return_mask_) << ")" << p_signature.function_name_ << "(";
	
	int arg_mask_count = (int)p_signature.arg_masks_.size();
	
	if (arg_mask_count == 0)
	{
		if (!p_signature.has_ellipsis_)
			p_outstream << "void";
	}
	else
	{
		for (int arg_index = 0; arg_index < arg_mask_count; ++arg_index)
		{
			ScriptValueMask type_mask = p_signature.arg_masks_[arg_index];
			
			if (arg_index > 0)
				p_outstream << ", ";
			
			p_outstream << StringForScriptValueMask(type_mask);
		}
	}
	
	if (p_signature.has_ellipsis_)
		p_outstream << ((arg_mask_count > 0) ? ", ..." : "...");
	
	p_outstream << ")";
	
	// if the function is provided by a delegate, show the delegate's name
	if (p_signature.delegate_name_.length())
		p_outstream << " <" << p_signature.delegate_name_ << ">";
	
	return p_outstream;
}

bool CompareFunctionSignatures(const FunctionSignature *i, const FunctionSignature *j)
{
	return (i->function_name_ < j->function_name_);
}


































































