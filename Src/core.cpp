/*
 * core.cpp
 *
 *  Created on: Aug 30, 2019
 *      Author: Alex
 */

#include <string.h>
#include "core.h"
#include "mode.h"
#include "stat.h"
#include "oled.h"
#include "tools.h"
#include "buzzer.h"

#define ADC_CONV 	(2)										// Activated ADC Ranks Number (hadc2.Init.NbrOfConversion)
#define ADC_LOOPS	(2)										// Number of ADC conversion loops. Should be even
#define ADC_BUFF_SZ	(2*ADC_CONV*ADC_LOOPS)

extern ADC_HandleTypeDef	hadc1;
extern ADC_HandleTypeDef	hadc2;
extern TIM_HandleTypeDef	htim2;

typedef enum { ADC_IDLE, ADC_CURRENT, ADC_TEMP } t_ADC_mode;
volatile static t_ADC_mode	adc_mode = ADC_IDLE;
volatile static uint16_t	buff[ADC_BUFF_SZ];
volatile static uint8_t		check_count	= 1;				// Decrement from check_period to zero by TIM2. When become zero, force to check the IRON connectivity

const static uint16_t  		max_iron_pwm	= 1960;			// Max value should be less than TIM2.CHANNEL3 value by 20
const static uint16_t		check_iron_pwm	= 5;			// This power should be applied to check the current through the IRON
const static uint8_t		check_period	= 6;			// TIM2 loops between check current through the iron

static HW		core;										// Hardware core (including all device instances)

// MODE instances
static	MSTBY_IRON		standby_iron(&core);
static	MWORK_IRON		work_iron(&core);
static	MBOOST			boost(&core);
static	MSLCT			select(&core);
static	MTACT			activate(&core);
static	MCALIB			calib_auto(&core);
static	MCALIB_MANUAL	calib_manual(&core);
static	MCALMENU		calib_menu(&core, &calib_auto, &calib_manual);
static	MTUNE			tune(&core);
static	MFAIL			fail(&core);
static	MMBST			boost_setup(&core);
static	MTPID			pid_tune(&core);
static	MAUTOPID		auto_pid_tune(&core);
static  MABOUT			about(&core);
static	MMENU			main_menu(&core, &boost_setup, &calib_menu, &activate, &tune, &pid_tune, &about);
static	MDEBUG			debug(&core);
static	MODE*           pMode = &standby_iron;


CFG_STATUS HW::init(void) {
	dspl.init(U8G2_R2);
	iron.init();
	encoder.addButton(ENCODER_B_GPIO_Port, ENCODER_B_Pin);
	CFG_STATUS cfg_init = 	cfg.init();
	PIDparam pp   		= 	cfg.pidParams();				// load IRON PID parameters
	iron.load(pp);
	buzz.activate(cfg.isBuzzerEnabled());
	return cfg_init;
}

extern "C" void setup(void) {
	CFG_STATUS cfg_init = core.init();						// Initialize the hardware structure before start timers

	HAL_ADCEx_Calibration_Start(&hadc1);					// Calibrate both ADCs
	HAL_ADCEx_Calibration_Start(&hadc2);
	HAL_TIM_PWM_Start(&htim2, 	TIM_CHANNEL_1);				// PWM signal of the IRON
	HAL_TIM_OC_Start_IT(&htim2,	TIM_CHANNEL_3);				// Check the current through the IRON
	HAL_TIM_OC_Start_IT(&htim2, TIM_CHANNEL_4);				// Calculate power of the IRON

	// Setup mode parameters: return mode, short press mode, long press mode
	standby_iron.setup(&select, &work_iron, &main_menu);
	work_iron.setup(&standby_iron, &standby_iron, &boost);
	boost.setup(&work_iron, &work_iron, &work_iron);
	select.setup(&standby_iron, &activate, &main_menu);
	activate.setup(&standby_iron, &standby_iron, &main_menu);
	calib_auto.setup(&standby_iron, &standby_iron, &standby_iron);
	calib_manual.setup(&calib_menu, &standby_iron, &standby_iron);
	calib_menu.setup(&standby_iron, &standby_iron, &standby_iron);
	tune.setup(&standby_iron, &standby_iron, &standby_iron);
	fail.setup(&standby_iron, &standby_iron, &standby_iron);
	boost_setup.setup(&main_menu, &main_menu, &standby_iron);
	pid_tune.setup(&standby_iron, &standby_iron, &standby_iron);
	auto_pid_tune.setup(&standby_iron, &pid_tune, &standby_iron);
	main_menu.setup(&standby_iron, &standby_iron, &standby_iron);
	about.setup(&standby_iron, &standby_iron, &debug);
    debug.setup(&standby_iron, &standby_iron, &standby_iron);

	switch (cfg_init) {
		case	CFG_NO_TIP:
			pMode	= &activate;							// No tip configured, run tip activation menu
			break;
		case	CFG_READ_ERROR:								// Failed to read EEPROM
			core.dspl.errorMessage("EEPROM\nread\nerror");
			pMode	= &fail;
			fail.setup(&fail, &fail, &fail);				// Stay in fail mode forever
			break;
		default:
			break;
	}

	HAL_Delay(500);											// Wait till hardware status updated
	pMode->init();
}


extern "C" void loop(void) {
	if (core.cfg.getLowTemp() > 0) {						// If low power temperature defined
		core.iron.checkSWStatus();							// Check status of IRON tilt switches
	}
	MODE* new_mode = pMode->returnToMain();
	if (new_mode && new_mode != pMode) {
		core.iron.switchPower(false);
		TIM2->CCR1	= 0;
		pMode = new_mode;
		pMode->init();
		return;
	}
	new_mode = pMode->loop();
	if (new_mode != pMode) {
		if (new_mode == 0) new_mode = &fail;				// Mode Failed
		core.iron.switchPower(false);
		TIM2->CCR1	= 0;
		pMode = new_mode;
		pMode->init();
	}
}

static bool adcStart(t_ADC_mode mode) {
    if (adc_mode != ADC_IDLE) {								// Not ready to check analog data; Something is wrong!!!
    	TIM2->CCR1 = 0;										// Switch off the IRON
		return false;
    }
	HAL_ADC_Start(&hadc2);
    HAL_ADCEx_MultiModeStart_DMA(&hadc1, (uint32_t*)buff, ADC_CONV*ADC_LOOPS);
	adc_mode = mode;
	return true;
}

/*
 * IRQ handler
 * on TIM2 Output channel #3 to read the current through the IRON and Fan of Hot Air Gun
 * also check that TIM1 counter changed driven by AC_ZERO interrupt
 * on TIM2 Output channel #4 to read the IRON, HOt Air Gun and ambient temperatures
 */
extern "C" void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim) {
	if (htim->Instance == TIM2) {
		if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3) {
			if (TIM2->CCR1)									// If IRON has been powered
				adcStart(ADC_CURRENT);
		} else if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_4) {
			adcStart(ADC_TEMP);
		}
	}
}

/*
 * IRQ handler of ADC complete request. The data is in the ADC buffer (buff)
 * Data read by 4 slots interleaved: adc1-rank1, adc2-rank1, adc1-rank2, adc2-rank2
 * The ADC buffer would have the following fields (see MX_ADC1_Init() MX_ADC2_Init() in main.c)
 * ADC1:			ADC2:
 * iron_current		iron_temp
 * ambient			iron_temp
 */
extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
	HAL_ADCEx_MultiModeStop_DMA(&hadc1);
	HAL_ADC_Stop(&hadc2);
	if (adc_mode == ADC_TEMP) {
		uint32_t iron_temp	= 0;
		uint32_t ambient	= 0;
		for (uint8_t i = 0; i < ADC_BUFF_SZ; i += 2*ADC_CONV) {
			iron_temp	+= buff[i+1] 	+ buff[i+3];
			ambient		+= buff[i+2];
		}
		iron_temp 	+= ADC_LOOPS;							// Round the result
		iron_temp 	/= ADC_LOOPS*2;
		ambient		+= ADC_LOOPS/2;
		ambient		/= ADC_LOOPS;
		core.iron.updateAmbient(ambient);

		uint8_t min_iron_pwm = 0;							// By default do not power the IRON to check connectivity
		if (--check_count == 0) {							// It is time to check IRON is connected or not
			check_count	 = check_period;
			min_iron_pwm = check_iron_pwm;
		}
		if (core.iron.isIronConnected()) {
			uint16_t iron_power = core.iron.power(iron_temp);
			TIM2->CCR1	= constrain(iron_power, min_iron_pwm, max_iron_pwm);

		} else {
			TIM2->CCR1	= min_iron_pwm;						// Sometimes supply minimum power to the IRON to check connectivity
		}
	} else if (adc_mode == ADC_CURRENT) {
		uint32_t iron_curr	= 0;
		for (uint8_t i = 0; i < ADC_BUFF_SZ; i += 2*ADC_CONV) {
			iron_curr	+= buff[i];
		}
		iron_curr	+= ADC_LOOPS/2;							// Round the result
		iron_curr	/= ADC_LOOPS;

		if (TIM2->CCR1)										// If IRON has been powered
			core.iron.updateIronCurrent(iron_curr);
	}
	adc_mode = ADC_IDLE;
}

extern "C" void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc) 				{ }
extern "C" void HAL_ADC_LevelOutOfWindowCallback(ADC_HandleTypeDef *hadc) 	{ }

// Encoder Rotated
extern "C" void EXTI0_IRQHandler(void) {
	core.encoder.encoderIntr();
	__HAL_GPIO_EXTI_CLEAR_IT(ENCODER_L_Pin);
}
