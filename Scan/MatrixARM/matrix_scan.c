/* Copyright (C) 2014-2015 by Jacob Alexander
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// ----- Includes -----

// Compiler Includes
#include <Lib/ScanLib.h>

// Project Includes
#include <cli.h>
#include <kll.h>
#include <led.h>
#include <print.h>
#include <macro.h>

// Local Includes
#include "matrix_scan.h"

// Matrix Configuration
#include <matrix.h>



// ----- Defines -----

// A neverending scan counter.
uint32_t Matrix_divCounter = 0;

// A minimum threshold in the value between two key presses and releases
// required in order to output the value. Warning: This was set empirically
// after inspecting the histogram of time differences and will be incorrect for
// non-zero values of DebounceThrottleDiv.
//
// FIXME: Estimate this automatically to avoid the need for hardcoding.
uint32_t Matrix_repeatThreshold = 16;

// Histograms to tally the times taken between key presses and releases. This is
// fed to the "matrixHist" routine for debugging.
uint32_t Matrix_timesRelease[32];
uint32_t Matrix_timesPress[32];


// ----- Function Declarations -----

// CLI Functions
void cliFunc_matrixDebug( char* args );
void cliFunc_matrixState( char* args );
void cliFunc_matrixHist( char* args );



// ----- Variables -----

// Scan Module command dictionary
CLIDict_Entry( matrixDebug,  "Enables matrix debug mode, prints out each scan code." NL "\t\tIf argument \033[35mT\033[0m is given, prints out each scan code state transition." );
CLIDict_Entry( matrixState,  "Prints out the current scan table N times." NL "\t\t \033[1mO\033[0m - Off, \033[1;33mP\033[0m - Press, \033[1;32mH\033[0m - Hold, \033[1;35mR\033[0m - Release, \033[1;31mI\033[0m - Invalid" );
CLIDict_Entry( matrixHist,   "Prints a histogram of the times between a key presss and releases." );

CLIDict_Def( matrixCLIDict, "Matrix Module Commands" ) = {
	CLIDict_Item( matrixDebug ),
	CLIDict_Item( matrixState ),
	CLIDict_Item( matrixHist ),
	{ 0, 0, 0 } // Null entry for dictionary end
};

// Debounce Array
KeyState Matrix_scanArray[ Matrix_colsNum * Matrix_rowsNum ];

// Matrix debug flag - If set to 1, for each keypress the scan code is displayed in hex
//                     If set to 2, for each key state change, the scan code is displayed along with the state
uint8_t matrixDebugMode = 0;

// Matrix State Table Debug Counter - If non-zero display state table after every matrix scan
uint16_t matrixDebugStateCounter = 0;

// Matrix Scan Counters
uint16_t matrixMaxScans  = 0;
uint16_t matrixCurScans  = 0;
uint16_t matrixPrevScans = 0;



// ----- Functions -----

// Pin action (Strobe, Sense, Strobe Setup, Sense Setup)
// NOTE: This function is highly dependent upon the organization of the register map
//       Only guaranteed to work with Freescale MK20 series uCs
uint8_t Matrix_pin( GPIO_Pin gpio, Type type )
{
	// Register width is defined as size of a pointer
	unsigned int gpio_offset = gpio.port * 0x40   / sizeof(unsigned int*);
	unsigned int port_offset = gpio.port * 0x1000 / sizeof(unsigned int*) + gpio.pin;

	// Assumes 0x40 between GPIO Port registers and 0x1000 between PORT pin registers
	// See Lib/mk20dx.h
	volatile unsigned int *GPIO_PDDR = (unsigned int*)(&GPIOA_PDDR) + gpio_offset;
	volatile unsigned int *GPIO_PSOR = (unsigned int*)(&GPIOA_PSOR) + gpio_offset;
	volatile unsigned int *GPIO_PCOR = (unsigned int*)(&GPIOA_PCOR) + gpio_offset;
	volatile unsigned int *GPIO_PDIR = (unsigned int*)(&GPIOA_PDIR) + gpio_offset;
	volatile unsigned int *PORT_PCR  = (unsigned int*)(&PORTA_PCR0) + port_offset;

	// Operation depends on Type
	switch ( type )
	{
	case Type_StrobeOn:
		*GPIO_PSOR |= (1 << gpio.pin);
		break;

	case Type_StrobeOff:
		*GPIO_PCOR |= (1 << gpio.pin);
		break;

	case Type_StrobeSetup:
		// Set as output pin
		*GPIO_PDDR |= (1 << gpio.pin);

		// Configure pin with slow slew, high drive strength and GPIO mux
		*PORT_PCR = PORT_PCR_SRE | PORT_PCR_DSE | PORT_PCR_MUX(1);

		// Enabling open-drain if specified
		switch ( Matrix_type )
		{
		case Config_Opendrain:
			*PORT_PCR |= PORT_PCR_ODE;
			break;

		// Do nothing otherwise
		default:
			break;
		}
		break;

	case Type_Sense:
		return *GPIO_PDIR & (1 << gpio.pin) ? 1 : 0;

	case Type_SenseSetup:
		// Set as input pin
		*GPIO_PDDR &= ~(1 << gpio.pin);

		// Configure pin with passive filter and GPIO mux
		*PORT_PCR = PORT_PCR_PFE | PORT_PCR_MUX(1);

		// Pull resistor config
		switch ( Matrix_type )
		{
		case Config_Pullup:
			*PORT_PCR |= PORT_PCR_PE | PORT_PCR_PS;
			break;

		case Config_Pulldown:
			*PORT_PCR |= PORT_PCR_PE;
			break;

		// Do nothing otherwise
		default:
			break;
		}
		break;
	}

	return 0;
}

// Setup GPIO pins for matrix scanning
void Matrix_setup()
{
	// Register Matrix CLI dictionary
	CLI_registerDictionary( matrixCLIDict, matrixCLIDictName );

	info_msg("Columns:  ");
	printHex( Matrix_colsNum );

	// Setup Strobe Pins
	for ( uint8_t pin = 0; pin < Matrix_colsNum; pin++ )
	{
		Matrix_pin( Matrix_cols[ pin ], Type_StrobeSetup );
	}

	print( NL );
	info_msg("Rows:     ");
	printHex( Matrix_rowsNum );

	// Setup Sense Pins
	for ( uint8_t pin = 0; pin < Matrix_rowsNum; pin++ )
	{
		Matrix_pin( Matrix_rows[ pin ], Type_SenseSetup );
	}

	print( NL );
	info_msg("Max Keys: ");
	printHex( Matrix_maxKeys );

	// Clear out Debounce Array
	for ( uint8_t item = 0; item < Matrix_maxKeys; item++ )
	{
		Matrix_scanArray[ item ].prevState = KeyState_Off;
		Matrix_scanArray[ item ].curState = KeyState_Off;
		Matrix_scanArray[ item ].activeCount = 0;
		Matrix_scanArray[ item ].inactiveCount = DebounceDivThreshold_define; // Start at 'off' steady state
		Matrix_scanArray[ item ].lastChangeCounter = 0;
	}

	// Clear scan stats counters
	matrixMaxScans  = 0;
	matrixPrevScans = 0;

	// Clear histogram counters.
	for (int i = 0; i < 32; ++i) {
		Matrix_timesRelease[i] = 0;
		Matrix_timesPress[i] = 0;
	}
}

void Matrix_keyPositionDebug( KeyPosition pos )
{
	// Depending on the state, use a different flag + color
	switch ( pos )
	{
	case KeyState_Off:
		print("\033[1mO\033[0m");
		break;

	case KeyState_Press:
		print("\033[1;33mP\033[0m");
		break;

	case KeyState_Hold:
		print("\033[1;32mH\033[0m");
		break;

	case KeyState_Release:
		print("\033[1;35mR\033[0m");
		break;

	case KeyState_Invalid:
	default:
		print("\033[1;31mI\033[0m");
		break;
	}
}


 // Scan the matrix for keypresses
// NOTE: scanNum should be reset to 0 after a USB send (to reset all the counters)
void Matrix_scan( uint16_t scanNum )
{
	++Matrix_divCounter;
    
#if ( DebounceThrottleDiv_define > 0 )
	// Scan-rate throttling
	// By scanning using a divider, the scan rate slowed down
	// DebounceThrottleDiv_define == 1 means -> /2 or half scan rate
	// This helps with bouncy switches on fast uCs
	if ( !( Matrix_divCounter & (1 << ( DebounceThrottleDiv_define - 1 )) ) )
		return;
#endif

 	// Increment stats counters
	if ( scanNum > matrixMaxScans ) {
		matrixMaxScans = scanNum;
	}
	if ( scanNum == 0 )
	{
		matrixPrevScans = matrixCurScans;
		matrixCurScans = 0;
	}
	else
	{
		matrixCurScans++;
	}

	// For each strobe, scan each of the sense pins
	for ( uint8_t strobe = 0; strobe < Matrix_colsNum; strobe++ )
	{
		// Strobe Pin
		Matrix_pin( Matrix_cols[ strobe ], Type_StrobeOn );

		// Scan each of the sense pins
		for ( uint8_t sense = 0; sense < Matrix_rowsNum; sense++ )
		{
			// Key position
			uint8_t key = Matrix_colsNum * sense + strobe;
			KeyState *state = &Matrix_scanArray[ key ];

			// If first scan, reset state
			if ( scanNum == 0 )
			{
				// Set previous state, and reset current state
				state->prevState = state->curState;
				state->curState  = KeyState_Invalid;
			}

			// Signal Detected
			// Increment count and right shift opposing count
			// This means there is a maximum of scan 13 cycles on a perfect off to on transition
			//  (coming from a steady state 0xFFFF off scans)
			// Somewhat longer with switch bounciness
			// The advantage of this is that the count is ongoing and never needs to be reset
			// State still needs to be kept track of to deal with what to send to the Macro module
			if ( Matrix_pin( Matrix_rows[ sense ], Type_Sense ) )
			{
				// Only update if not going to wrap around
				if ( state->activeCount < DebounceDivThreshold_define ) {
					state->activeCount += 1;
				}
				state->inactiveCount >>= 1;
			}
			// Signal Not Detected
			else
			{
				// Only update if not going to wrap around
				if ( state->inactiveCount < DebounceDivThreshold_define ) {
					state->inactiveCount += 1;
				}
				state->activeCount >>= 1;
			}

			// Check for state change if it hasn't been set
			// Only check if the minimum number of scans has been met
			//   the current state is invalid
			//   and either active or inactive count is over the debounce threshold
			if ( state->curState == KeyState_Invalid )
			{
				// Compute the time difference since the last
				// change.
				uint32_t diff = Matrix_divCounter - state->lastChangeCounter;

				// Compute the corresponding histogram bucket
				// (as a power of two).
				uint8_t bucket = 31 - __builtin_clzl(diff);
				
				// Ignore this event if it occurs too fast in a
				// sequence. This is necessary because the
				// debouncing logic appears to be trumped by
				// some sequence of events. FIXME: Review this.
				if ( diff < Matrix_repeatThreshold ) {
					state->curState = state->prevState;
					continue;
				}

				switch ( state->prevState )
				{
				case KeyState_Press:
				case KeyState_Hold:
					if ( state->activeCount > state->inactiveCount )
					{
						state->curState = KeyState_Hold;
					}
					else
					{
						state->curState = KeyState_Release;
						Matrix_timesRelease[bucket]++;
						state->lastChangeCounter = Matrix_divCounter;
					}
					break;

				case KeyState_Release:
				case KeyState_Off:
					if ( state->activeCount > state->inactiveCount )
					{
						state->curState = KeyState_Press;
						Matrix_timesPress[bucket]++;
						state->lastChangeCounter = Matrix_divCounter;
					}
					else
					{
						state->curState = KeyState_Off;
					}
					break;

				case KeyState_Invalid:
				default:
					erro_print("Matrix scan bug!! Report me!");
					break;
				}

				// Send keystate to macro module
				Macro_keyState( key, state->curState );

				// Matrix Debug, only if there is a state change
				if ( matrixDebugMode && state->curState != state->prevState )
				{
					// Basic debug output
					if ( matrixDebugMode == 1 && state->curState == KeyState_Press )
					{
						printHex( key );
						print(" ");
					}
					// State transition debug output
					else if ( matrixDebugMode == 2 )
					{
						printHex( key );
						Matrix_keyPositionDebug( state->curState );
						print(" ");
					}
				}
			}
		}

		// Unstrobe Pin
		Matrix_pin( Matrix_cols[ strobe ], Type_StrobeOff );
	}

	// State Table Output Debug
	if ( matrixDebugStateCounter > 0 )
	{
		// Decrement counter
		matrixDebugStateCounter--;

		// Output stats on number of scans being done per USB send
		print( NL );
		info_msg("Max scans:      ");
		printHex( matrixMaxScans );
		print( NL );
		info_msg("Previous scans: ");
		printHex( matrixPrevScans );
		print( NL );

		// Output current scan number
		info_msg("Scan Number:    ");
		printHex( scanNum );
		print( NL );

		// Display the state info for each key
		print("<key>:<previous state><current state> <active count> <inactive count>");
		for ( uint8_t key = 0; key < Matrix_maxKeys; key++ )
		{
			// Every 4 keys, put a newline
			if ( key % 4 == 0 )
				print( NL );

			print("\033[1m0x");
			printHex_op( key, 2 );
			print("\033[0m");
			print(":");
			Matrix_keyPositionDebug( Matrix_scanArray[ key ].prevState );
			Matrix_keyPositionDebug( Matrix_scanArray[ key ].curState );
			print(" 0x");
			printHex_op( Matrix_scanArray[ key ].activeCount, 4 );
			print(" 0x");
			printHex_op( Matrix_scanArray[ key ].inactiveCount, 4 );
			print(" ");
		}

		print( NL );
	}
}


// ----- CLI Command Functions -----

void cliFunc_matrixDebug ( char* args )
{
	// Parse number from argument
	//  NOTE: Only first argument is used
	char* arg1Ptr;
	char* arg2Ptr;
	CLI_argumentIsolation( args, &arg1Ptr, &arg2Ptr );

	// Set the matrix debug flag depending on the argument
	// If no argument, set to scan code only
	// If set to T, set to state transition
	switch ( arg1Ptr[0] )
	{
	// T as argument
	case 'T':
	case 't':
		matrixDebugMode = matrixDebugMode != 2 ? 2 : 0;
		break;

	// No argument
	case '\0':
		matrixDebugMode = matrixDebugMode != 1 ? 1 : 0;
		break;

	// Invalid argument
	default:
		return;
	}

	print( NL );
	info_msg("Matrix Debug Mode: ");
	printInt8( matrixDebugMode );
}

void cliFunc_matrixState ( char* args )
{
	// Parse number from argument
	//  NOTE: Only first argument is used
	char* arg1Ptr;
	char* arg2Ptr;
	CLI_argumentIsolation( args, &arg1Ptr, &arg2Ptr );

	// Default to 1 if no argument is given
	matrixDebugStateCounter = 1;

	if ( arg1Ptr[0] != '\0' )
	{
		matrixDebugStateCounter = (uint16_t)numToInt( arg1Ptr );
	}
}

void cliFunc_matrixHist ( char* args )
{
	print("Histogram\r\n");
	for (int8_t i = 0; i < 32; ++i) {
		printInt8(i);
		print(" ");
		printInt32(Matrix_timesRelease[i]);
		print(" ");
		printInt32(Matrix_timesPress[i]);
		print(NL);
	}
}

