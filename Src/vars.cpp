/*
 * vars.cpp
 *
 *  Created on: 23 ����. 2019 �.
 *      Author: Alex
 */

#include "vars.h"

const uint16_t	int_temp_max				= 3700;			// Maximum possible temperature in internal units

const uint8_t	auto_pid_hist_length		= 16;			// The history data length of PID tuner average values
const uint8_t	ec							= 200;			// Exponential average coefficient (default value)

const uint16_t	iron_temp_minC				= 180;			// Minimum IRON calibration temperature in degrees of Celsius
const uint16_t 	iron_temp_maxC 				= 450;			// Maximum IRON calibration temperature in degrees of Celsius

const uint8_t	default_ambient				= 25;
