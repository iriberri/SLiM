//
//  EidosTextViewDelegate.h
//  EidosScribe
//
//  Created by Ben Haller on 9/10/15.
//  Copyright (c) 2015 Philipp Messer.  All rights reserved.
//	A product of the Messer Lab, http://messerlab.org/software/
//

//	This file is part of Eidos.
//
//	Eidos is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
//
//	Eidos is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License along with Eidos.  If not, see <http://www.gnu.org/licenses/>.

#import <Cocoa/Cocoa.h>

#include "eidos_symbol_table.h"

@class EidosTextView;


/*
 
 A protocol of optional methods that EidosTextView's delegate can implement, to
 allow the Context to customize code completion in an EidosTextView.  Note that
 if EidosConsoleWindowController is used, these methods get forwarded on by its
 delegate as well, so that EidosConsoleWindowController also gets Context-defined
 behavior.

 */

@protocol EidosTextViewDelegate <NSObject>
@optional

// Provide a global symbol table to use for completion; EidosConsoleWindowController
// provides the symbol table for the ongoing interpreter context, for example
- (EidosSymbolTable *)eidosTextViewGlobalSymbolTableForCompletion:(EidosTextView *)eidosTextView;

// Supply additional language keywords that should be completed; used if the Context
// extends the grammar of Eidos, otherwise can be unimplemented
- (NSArray *)eidosTextViewLanguageKeywordsForCompletion:(EidosTextView *)eidosTextView;

// Supply additional function signatures for functions added by the Context, if any;
// otherwise can be unimplemented
- (const std::vector<const EidosFunctionSignature*> *)eidosTextViewInjectedFunctionSignatures:(EidosTextView *)eidosTextView;

// Supply all method signatures for all methods of all classes; used to show the
// signature for the currently editing method call in the status bar (note that
// this will not work perfectly if the Context defines multiple methods of the same
// name but with different signatures)
- (const std::vector<const EidosMethodSignature*> *)eidosTextViewAllMethodSignatures:(EidosTextView *)eidosTextView;

// This allows the Context to define some special identifier tokens that should
// receive different syntax coloring from standard identifiers because they are
// in some way built in or provided by the Context automatically
- (bool)eidosTextView:(EidosTextView *)eidosTextView tokenStringIsSpecialIdentifier:(const std::string &)token_string;

@end




































