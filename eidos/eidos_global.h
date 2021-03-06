//
//  eidos_global.h
//  Eidos
//
//  Created by Ben Haller on 6/28/15.
//  Copyright (c) 2015-2016 Philipp Messer.  All rights reserved.
//	A product of the Messer Lab, http://messerlab.org/slim/
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


#ifndef __Eidos__eidos_global__
#define __Eidos__eidos_global__

#include <stdio.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

#if ((defined(SLIMGUI) && (SLIMPROFILING == 1)) || defined(EIDOS_GUI))
#include <mach/mach_time.h>		// for mach_absolute_time(), for profiling; needed only in SLiMgui and the Eidos GUI (the latter for the timing test code)
#endif

class EidosScript;
class EidosToken;


// This should be called once at startup to give Eidos an opportunity to initialize static state
void Eidos_WarmUp(void);

// This can be called at startup, after Eidos_WarmUp(), to define global constants from the command line
void Eidos_DefineConstantsFromCommandLine(std::vector<std::string> p_constants);


// *******************************************************************************************************************
//
//	Context customization
//
#pragma mark -
#pragma mark Context customization

// Information on the Context within which Eidos is running (if any).  This is basically a way to let the Context
// customize the version and license and citation information printed by Eidos.

extern std::string gEidosContextVersion;
extern std::string gEidosContextLicense;
extern std::string gEidosContextCitation;

// This is a vector of the classes defined by the Context.  This is used to translate from a string representation
// of a class, as in a type-specifier in a function declaration, to the corresponding class object (i.e., subclass
// of EidosObjectClass).  Because this is a global, a given process may at present have only one Context, with a
// list of class shared by Eidos across that process.  At some point we probably want to further formalize the
// Context as an object that is passed in to Eidos when needed, allowing different Contexts to be used within the
// same process.
class EidosObjectClass;

extern std::vector<EidosObjectClass *> gEidosContextClasses;


// *******************************************************************************************************************
//
//	Error tracking
//
#pragma mark -
#pragma mark Error tracking

// The part of the input file that caused an error; used to highlight the token or text that caused the error.
// Eidos now also supports reporting of errors with quoted script lines, using the EidosScript* here.  The
// error tracking and reporting stuff is unfortunately very fragile, because it is based on global variables
// that get magically set up in various places and then get used in various completely different places.  This
// is a big reason why Eidos is not thread-safe at present, and it's one of the trickiest parts of the code,
// for no very good reason except that I haven't yet figured out the right way to fix it.  FIXME
extern int gEidosCharacterStartOfError, gEidosCharacterEndOfError;
extern int gEidosCharacterStartOfErrorUTF16, gEidosCharacterEndOfErrorUTF16;
extern EidosScript *gEidosCurrentScript;
extern bool gEidosExecutingRuntimeScript;

extern int gEidosErrorLine, gEidosErrorLineCharacter;	// set up by eidos_terminate()


// *******************************************************************************************************************
//
//	Debugging support
//
#pragma mark -
#pragma mark Debugging support

// Debugging #defines that can be turned on
#define EIDOS_DEBUG_COMPLETION	0	// turn on to log information about symbol types whenever doing code completion

// Flags for various runtime checks that can be turned on or off; in SLiM, -x turns these off.
extern bool eidos_do_memory_checks;


// *******************************************************************************************************************
//
//	Memory usage monitoring
//
#pragma mark -
#pragma mark Memory usage monitoring

// Memory-monitoring calls.  See the .cpp for comments.  These return a size in bytes.
size_t EidosGetPeakRSS(void);
size_t EidosGetCurrentRSS(void);

// Memory limits, retrieved by calling "ulimit -m"; cached internally.  Returns a size in bytes; 0 means "no limit".
std::string EidosExec(const char* cmd);
size_t EidosGetMaxRSS(void);

// This checks whether our memory usage has gotten within 10 MB of the maximum memory usage, and terminates if so.
// p_message1 should be the name of the calling function/method; p_message2 can be any clarifying message.
// It is a good idea to check eidos_do_memory_checks before calling this, to save calling overhead.
void EidosCheckRSSAgainstMax(std::string p_message1, std::string p_message2);


// *******************************************************************************************************************
//
//	Profiling suuport
//
#pragma mark -
#pragma mark Profiling support

#if ((defined(SLIMGUI) && (SLIMPROFILING == 1)) || defined(EIDOS_GUI))
// PROFILING

extern int gEidosProfilingClientCount;	// if non-zero, profiling is happening in some context

// Profiling clocks; note that these can overflow, we don't care, only (t2-t1) ever matters and that is overflow-robust

// This is the fastest clock, is available across OS X versions, and gives us nanoseconds.  The only disadvantage to
// it is that it is platform-specific, so we can only use this clock in SLiMgui and Eidos_GUI.  That is OK.  This
// returns uint64_t in CPU-specific time units; see https://developer.apple.com/library/content/qa/qa1398/_index.html
typedef uint64_t eidos_profile_t;

extern uint64_t gEidos_ProfileCounter;			// incremented by Eidos_ProfileTime() every time it is called
extern double gEidos_ProfileOverheadTicks;		// the overhead in ticks for one profile call, in ticks
extern double gEidos_ProfileOverheadSeconds;	// the overhead in ticks for one profile call, in seconds
extern double gEidos_ProfileLagTicks;			// the clocked length of an empty profile block, in ticks
extern double gEidos_ProfileLagSeconds;			// the clocked length of an empty profile block, in seconds

// Get a profile clock measurement, to be used as a start or end time
inline eidos_profile_t Eidos_ProfileTime(void) { gEidos_ProfileCounter++; return mach_absolute_time(); }

// Convert an elapsed profiling time (the difference between two Eidos_ProfileTime() results) to seconds
double Eidos_ElapsedProfileTime(uint64_t p_elapsed_profile_time);

// This should be called immediately before profiling to measure the overhead and lag for profile blocks
void Eidos_PrepareForProfiling(void);

// Macros for profile blocks
#define	SLIM_PROFILE_BLOCK_START()																																			\
	bool slim__condition_a = (gEidosProfilingClientCount ? true : false);																									\
	eidos_profile_t slim__start_clock = (slim__condition_a ? Eidos_ProfileTime() : 0);																						\
	uint64_t slim__start_profile_counter = (slim__condition_a ? gEidos_ProfileCounter : 0);

#define	SLIM_PROFILE_BLOCK_START_NESTED()																																	\
	eidos_profile_t slim__start_clock2 = (slim__condition_a ? Eidos_ProfileTime() : 0);																						\
	uint64_t slim__start_profile_counter2 = (slim__condition_a ? gEidos_ProfileCounter : 0);

#define	SLIM_PROFILE_BLOCK_START_CONDITION(slim__condition_param)																											\
	bool slim__condition_b = gEidosProfilingClientCount && (slim__condition_param);																							\
	eidos_profile_t slim__start_clock = (slim__condition_b ? Eidos_ProfileTime() : 0);																						\
	uint64_t slim__start_profile_counter = (slim__condition_b ? gEidos_ProfileCounter : 0);

#define SLIM_PROFILE_BLOCK_END(slim__accumulator)																															\
	if (slim__condition_a)																																					\
	{																																										\
		uint64_t slim__end_profile_counter = gEidos_ProfileCounter;																											\
		uint64_t slim__contained_profile_calls = slim__end_profile_counter - slim__start_profile_counter;																	\
		eidos_profile_t slim__end_clock = Eidos_ProfileTime();																												\
																																											\
		uint64_t slim__uncorrected_ticks = (slim__end_clock - slim__start_clock);																							\
		uint64_t slim__correction = (eidos_profile_t)round(gEidos_ProfileLagTicks + gEidos_ProfileOverheadTicks * slim__contained_profile_calls);							\
		uint64_t slim__corrected_ticks = ((slim__correction < slim__uncorrected_ticks) ? (slim__uncorrected_ticks - slim__correction) : 0);									\
																																											\
		(slim__accumulator) += slim__corrected_ticks;																														\
	}

#define SLIM_PROFILE_BLOCK_END_NESTED(slim__accumulator)																													\
	if (slim__condition_a)																																					\
	{																																										\
		uint64_t slim__end_profile_counter2 = gEidos_ProfileCounter;																										\
		uint64_t slim__contained_profile_calls2 = slim__end_profile_counter2 - slim__start_profile_counter2;																\
		eidos_profile_t slim__end_clock2 = Eidos_ProfileTime();																												\
																																											\
		uint64_t slim__uncorrected_ticks2 = (slim__end_clock2 - slim__start_clock2);																						\
		uint64_t slim__correction2 = (eidos_profile_t)round(gEidos_ProfileLagTicks + gEidos_ProfileOverheadTicks * slim__contained_profile_calls2);							\
		uint64_t slim__corrected_ticks2 = ((slim__correction2 < slim__uncorrected_ticks2) ? (slim__uncorrected_ticks2 - slim__correction2) : 0);							\
																																											\
		(slim__accumulator) += slim__corrected_ticks2;																														\
	}

#define SLIM_PROFILE_BLOCK_END_CONDITION(slim__accumulator)																													\
	if (slim__condition_b)																																					\
	{																																										\
		uint64_t slim__end_profile_counter = gEidos_ProfileCounter;																											\
		uint64_t slim__contained_profile_calls = slim__end_profile_counter - slim__start_profile_counter;																	\
		eidos_profile_t slim__end_clock = Eidos_ProfileTime();																												\
																																											\
		uint64_t slim__uncorrected_ticks = (slim__end_clock - slim__start_clock);																							\
		uint64_t slim__correction = (eidos_profile_t)round(gEidos_ProfileLagTicks + gEidos_ProfileOverheadTicks * slim__contained_profile_calls);							\
		uint64_t slim__corrected_ticks = ((slim__correction < slim__uncorrected_ticks) ? (slim__uncorrected_ticks - slim__correction) : 0);									\
																																											\
		(slim__accumulator) += slim__corrected_ticks;																														\
	}

#endif


// *******************************************************************************************************************
//
//	Termination handling
//
#pragma mark -
#pragma mark Termination handling

// Print a demangled stack backtrace of the caller function to FILE* out; see eidos_global.cpp for credits and comments.
void eidos_print_stacktrace(FILE *p_out = stderr, unsigned int p_max_frames = 63);

// Print an offending line of script with carets indicating an error position
void eidos_script_error_position(int p_start, int p_end, EidosScript *p_script);
void eidos_log_script_error(std::ostream& p_out, int p_start, int p_end, EidosScript *p_script, bool p_inside_lambda);

// If gEidosTerminateThrows == 0, << eidos_terminate causes a call to exit().  In that mode, output
// related to termination output goes to cerr.  The other mode has gEidosTerminateThrows == 1.  In that mode,
// we use a global ostringstream to capture all termination-related output, and whoever catches the raise handles
// the termination stream.  All other Eidos output goes to ExecutionOutputStream(), defined on EidosInterpreter.
extern bool gEidosTerminateThrows;
extern std::ostringstream gEidosTermination;

#define EIDOS_TERMINATION	(gEidosTerminateThrows ? gEidosTermination : std::cerr)

// This little class is used as a stream manipulator that causes termination with EXIT_FAILURE, optionally
// with a backtrace.  This is nice since it lets us log and terminate in a single line of code.  It also allows
// a GUI to intercept the exit() call and do something more graceful with it.
class eidos_terminate
{
public:
	bool print_backtrace_ = false;
	
	eidos_terminate(void) = default;							// default constructor, no backtrace, does not change error range
	explicit eidos_terminate(const EidosToken *p_error_token);	// supply a token from which an error range is taken
	
	// These constructors request a backtrace as well
	explicit eidos_terminate(bool p_print_backtrace);
	eidos_terminate(const EidosToken *p_error_token, bool p_print_backtrace);
};

// Send an eidos_terminate object to an output stream, causing a raise or an exit() call depending on
// gEidosTerminateThrows.  Either way, this call does not return, so it is marked as noreturn as a hint
// to the compiler.  Also, since this is always an error case, it is marked as cold as a hint to branch
// prediction and optimization; all code paths that lead to this will be considered cold.
void operator<<(std::ostream& p_out, const eidos_terminate &p_terminator) __attribute__((__noreturn__)) __attribute__((cold));

// Get the message from the last raise out of gEidosTermination, optionally with newlines trimmed from the ends
std::string EidosGetTrimmedRaiseMessage(void);
std::string EidosGetUntrimmedRaiseMessage(void);


// *******************************************************************************************************************
//
//	Utility functions
//
#pragma mark -
#pragma mark Utility functions

// Resolve a leading ~ in a filesystem path to the user's home directory
std::string EidosResolvedPath(const std::string p_path);

// Create a temporary file based upon a template filename; note that pattern is modified!
int Eidos_mkstemps(char *pattern, int suffix_len);

// Welch's t-test functions; sample means are returned in mean1 and mean2, which may be nullptr
double Eidos_TTest_TwoSampleWelch(const double *set1, int count1, const double *set2, int count2, double *mean1, double *mean2);
double Eidos_TTest_OneSample(const double *set1, int count1, double mu, double *mean1);

// Exact summation of a floating-point vector using the Shewchuk algorithm; surprisingly, not in the GSL
double Eidos_ExactSum(const double *double_vec, int64_t vec_length);


// *******************************************************************************************************************
//
//	Overflow-detecting integer operations
//
#pragma mark -
#pragma mark Overflow-detecting integer operations

// Clang and GCC 5.0 support a suite a builtin functions that perform integer operations with detection of overflow.
//
//     http://clang.llvm.org/docs/LanguageExtensions.html#checked-arithmetic-builtins and
//     https://gcc.gnu.org/onlinedocs/gcc/Integer-Overflow-Builtins.html
//
// We want to use those builtins when possible, so that Eidos integer arithmetic is robust against overflow bugs.
// However, since GCC pre-5.0 doesn't have these builtins, and other compilers probably don't either, we have to
// do some messy version checking and such.

// BCH 6 April 2016: switched from the type-specific variants (i.e. __builtin_saddll_overflow, etc.) to the
// type-non-specific variants (i.e. __builtin_add_overflow) since some platforms use more than 64 bits for
// "long long".  The non-specific variants should choose which type-specific variant to use at compile time
// anyway, so this should not be an issue as long as we are careful to always use int64_t operands.

#if (defined(__clang__) && defined(__has_builtin))

#if __has_builtin(__builtin_add_overflow)
// Clang with support for overflow built-ins (if one is defined we assume all of them are)
#define Eidos_add_overflow(a, b, c)	__builtin_add_overflow((a), (b), (c))
#define Eidos_sub_overflow(a, b, c)	__builtin_sub_overflow((a), (b), (c))
#define Eidos_mul_overflow(a, b, c)	__builtin_mul_overflow((a), (b), (c))
#endif

#elif defined(__GNUC__)

#if (__GNUC__ >= 5)
// GCC 5.0 or later; overflow built-ins supported
#define Eidos_add_overflow(a, b, c)	__builtin_add_overflow((a), (b), (c))
#define Eidos_sub_overflow(a, b, c)	__builtin_sub_overflow((a), (b), (c))
#define Eidos_mul_overflow(a, b, c)	__builtin_mul_overflow((a), (b), (c))
#endif

#endif

// Uncomment these to test handling of missing built-ins on a system that does have the built-ins; with these
// uncommented, the Eidos test suite should emit a warning about missing integer overflow checks, but all tests
// that are run should pass.
//#undef Eidos_add_overflow
//#undef Eidos_sub_overflow
//#undef Eidos_mul_overflow


#ifdef Eidos_add_overflow

#define EIDOS_HAS_OVERFLOW_BUILTINS		1

#else

// Define the macros to just use simple operators, in all other cases.  Note that this means overflows are not
// detected!  The Eidos test suite will emit a warning in this case, telling the user to upgrade their compiler.
#define Eidos_add_overflow(a, b, c)	(*(c)=(a)+(b), false)
#define Eidos_sub_overflow(a, b, c)	(*(c)=(a)-(b), false)
#define Eidos_mul_overflow(a, b, c)	(*(c)=(a)*(b), false)
#define EIDOS_HAS_OVERFLOW_BUILTINS		0

#endif


// *******************************************************************************************************************
//
//	Global strings & IDs
//
#pragma mark -
#pragma mark Global strings & IDs

//
//	Global std::string objects.  This is kind of gross, but there are several rationales for it.  First of all, it makes
//	a speed difference; converting a C string to a std::string is done every time it is hit in the code (C++ does not
//	treat that as a constant expression and cache it for you, at least with the current generation of compilers).  The
//	conversion is surprisingly slow; it has shown up repeatedly in profiles I have done.  Second, there is the issue of
//	uniqueness; many of these strings occur in multiple places in the code, and a typo in one of those multiple occurrences
//	would cause a bug that would be very difficult to find.  If multiple places in the code intend to refer to the same
//	conceptual string, and rely on those references being the same, then a shared constant should be used.  So... oh well.
//

typedef uint32_t EidosGlobalStringID;

// EidosGlobalStringIDForString() takes any string and uniques through a hash table.  If the string does not already exist
// in the hash table, it is copied, and the copy is registered and returned as the uniqed string.
EidosGlobalStringID EidosGlobalStringIDForString(const std::string &p_string);

// StringForEidosGlobalStringID() returns the uniqued global string for the ID through a reverse lookup.  The reference
// returned is to the uniqued string stored internally by the hash table, so it will be the same every time this is called,
// and does not need to be copied if kept externally.
const std::string &StringForEidosGlobalStringID(EidosGlobalStringID p_string_id);

// This registers a standard string with a given ID; called by Eidos_RegisterGlobalStringsAndIDs(), and can be used by
// the Context to set up strings that need to have fixed IDs.  All IDs used by the Context should start at gEidosID_LastEntry.
// This function does not make a copy of the string that it is passed, since it is intended for use with global strings.
void Eidos_RegisterStringForGlobalID(const std::string &p_string, EidosGlobalStringID p_string_id);

// This registers all of the standard Eidos strings, listed below, without copying; the string global is the uniqued string.
void Eidos_RegisterGlobalStringsAndIDs(void);

// Frees all copied strings in the global string registry; used to help Valgrind understand what we're doing
void EidosFreeGlobalStrings(void);


extern const std::string gEidosStr_empty_string;
extern const std::string gEidosStr_space_string;

extern const std::string gEidosStr_apply;
extern const std::string gEidosStr_doCall;
extern const std::string gEidosStr_executeLambda;
extern const std::string gEidosStr__executeLambda_OUTER;
extern const std::string gEidosStr_ls;
extern const std::string gEidosStr_rm;

extern const std::string gEidosStr_if;
extern const std::string gEidosStr_else;
extern const std::string gEidosStr_do;
extern const std::string gEidosStr_while;
extern const std::string gEidosStr_for;
extern const std::string gEidosStr_in;
extern const std::string gEidosStr_next;
extern const std::string gEidosStr_break;
extern const std::string gEidosStr_return;
extern const std::string gEidosStr_function;

extern const std::string gEidosStr_T;
extern const std::string gEidosStr_F;
extern const std::string gEidosStr_NULL;
extern const std::string gEidosStr_PI;
extern const std::string gEidosStr_E;
extern const std::string gEidosStr_INF;
extern const std::string gEidosStr_MINUS_INF;	// "-INF"
extern const std::string gEidosStr_NAN;

extern const std::string gEidosStr_void;
extern const std::string gEidosStr_logical;
extern const std::string gEidosStr_string;
extern const std::string gEidosStr_integer;
extern const std::string gEidosStr_float;
extern const std::string gEidosStr_object;
extern const std::string gEidosStr_numeric;

extern const std::string gEidosStr_size;
extern const std::string gEidosStr_methodSignature;
extern const std::string gEidosStr_propertySignature;
extern const std::string gEidosStr_str;

extern const std::string gEidosStr_GetPropertyOfElements;
extern const std::string gEidosStr_ExecuteInstanceMethod;
extern const std::string gEidosStr_undefined;
extern const std::string gEidosStr_applyValue;

extern const std::string gEidosStr__TestElement;
extern const std::string gEidosStr__yolk;
extern const std::string gEidosStr__increment;
extern const std::string gEidosStr__cubicYolk;
extern const std::string gEidosStr__squareTest;

extern const std::string gEidosStr_weights;
extern const std::string gEidosStr_n;
extern const std::string gEidosStr_x;
extern const std::string gEidosStr_y;
extern const std::string gEidosStr_z;
extern const std::string gEidosStr_color;

extern const std::string gEidosStr_Mutation;	// in Eidos for hack reasons; see EidosValue_Object::EidosValue_Object()


// Not all global strings have a EidosGlobalStringID; basically just ones that we want to scan and pre-cache in the tree,
// such as property and method names, as well as initialize...() function names (since signatures can't be cached for them).
enum _EidosGlobalStringID : uint32_t
{
	gEidosID_none = 0,
	
	gEidosID_methodSignature,
	gEidosID_size,
	gEidosID_propertySignature,
	gEidosID_str,
	gEidosID_applyValue,
	
	gEidosID_T,
	gEidosID_F,
	gEidosID_NULL,
	gEidosID_PI,
	gEidosID_E,
	gEidosID_INF,
	gEidosID_NAN,
	
	gEidosID__TestElement,
	gEidosID__yolk,
	gEidosID__increment,
	gEidosID__cubicYolk,
	gEidosID__squareTest,
	
	gEidosID_weights,
	gEidosID_n,
	gEidosID_x,
	gEidosID_y,
	gEidosID_z,
	gEidosID_color,
	
	gEidosID_Mutation,		// in Eidos for hack reasons; see EidosValue_Object::EidosValue_Object()
	
	gEidosID_LastEntry,					// IDs added by the Context should start here
	gEidosID_LastContextEntry = 10000	// IDs added by the Context must end before this value; Eidos reserves the remaining values
};

extern std::vector<std::string> gEidosConstantNames;	// T, F, NULL, PI, E, INF, NAN


// *******************************************************************************************************************
//
//	Support for named / specified colors in SLiM
//
#pragma mark -
#pragma mark Named/specified color support

typedef struct {
	const char *name;
	uint8_t red, green, blue;
} EidosNamedColor;

extern EidosNamedColor gEidosNamedColors[];

void EidosGetColorComponents(const std::string &p_color_name, float *p_red_component, float *p_green_component, float *p_blue_component);
void EidosGetColorComponents(const std::string &p_color_name, uint8_t *p_red_component, uint8_t *p_green_component, uint8_t *p_blue_component);

void EidosGetColorString(double p_red, double p_green, double p_blue, char *p_string_buffer);	// p_string_buffer must have room for 8 chars, including the null


#endif /* defined(__Eidos__eidos_global__) */



















































