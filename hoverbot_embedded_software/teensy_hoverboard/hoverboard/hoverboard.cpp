/******************************************************************************
 * @file    hoverboard.cpp
 * @author  Rémi Pincent - INRIA
 * @date    28/11/2016
 *
 * @brief Hoverboard implementation. Hoverboard control done hacking : 
 *  - gyroscope daughterboards signals to control speed
 *  - power button
 *  - Brushless motors Hall sensors (3 per motors)
 *  - Status LED to get ERROR signal
 *  
 * Requires : 
 *  - 1 UART to simulate daughterboard gyroscope 1
 *  - 1 UART to simulater daughterboard gyroscope 2
 *  - 3x2 digital INPUTS to get Hall sensors pulses
 *  - 1 digital OUTPUT to simulate power button press
 *  - 1 digital INPUT to get ERROR status
 *  
 * Project : hoverbot
 * Contact:  Rémi Pincent - remi.pincent@inria.fr
 *
 * Revision History:
 * https://github.com/OpHaCo/hoverbot.git
 * 
 * LICENSE :
 * hoverbot (c) by Rémi Pincent
 * project_name is licensed under a
 * Creative Commons Attribution-NonCommercial 3.0 Unported License.
 *
 * You should have received a copy of the license along with this
 * work.  If not, see <http://creativecommons.org/licenses/by-nc/3.0/>.
 *****************************************************************************/

/**************************************************************************
 * Include Files
 **************************************************************************/
#include "hoverboard.h"

#include <logger_config.h>
#include <IntervalTimer.h>
#include <logger.h>

/**************************************************************************
 * Manifest Constants
 **************************************************************************/

/**************************************************************************
 * Type Definitions
 **************************************************************************/

/**************************************************************************
 * Static members definitions
 **************************************************************************/
const uint8_t Hoverboard::SHORT_PRESS_DUR_MS = 100;
const uint8_t Hoverboard::GYRO_FRAME_LENGTH = 6;
const uint8_t Hoverboard::GYRO_CONTACT_CLOSED_BYTE = 85;
const uint8_t Hoverboard::GYRO_CONTACT_OPENED_BYTE = 170;
const uint16_t Hoverboard::GYRO_FRAME_START = 256;

Hoverboard* Hoverboard::_instance = NULL;

/**************************************************************************
 * Public Methods Definitions
 **************************************************************************/
Hoverboard::Hoverboard(const Hoverboard::Config& arg_config):
  _u16_powerPin(arg_config._u16_powerPin),
  _u16_powerStatPin(arg_config._u16_powerStatPin),
  _timer(),
  _e_state(OUT_OF_ENUM_STATE),
	_p_gyro1Serial(arg_config._p_gyro1Serial),
	_p_gyro2Serial (arg_config._p_gyro2Serial),
  _f_speed1(0.0),
  _f_speed2(0.0),
  _f_sensorSpeed1(0.0), 
  _f_sensorSpeed2(0.0), 
  _f_diffSpeed(0.0),
  _s16_speedRampUp(0),
  _f_commonSpeed(0.0), 
  _p_listener(NULL),
  _u32_events(0)
{
  if(arg_config._p_motor1Conf)
  {
    arg_config._p_motor1Conf->_itCbs._pfn_hall1It = Hoverboard::motor1Hall1It;
    arg_config._p_motor1Conf->_itCbs._pfn_hall2It = Hoverboard::motor1Hall2It;
    arg_config._p_motor1Conf->_itCbs._pfn_hall3It = Hoverboard::motor1Hall3It;
    arg_config._p_motor1Conf->_itCbs._pfn_timerIt = Hoverboard::motor1TimerIt;
    _hallSensor1 = BrushlessHallSensor(*arg_config._p_motor1Conf);
    _has_motor1HallSensor = true;
  }
  else
  {
    /** Do not use sensor */
    _has_motor1HallSensor = false;
  }
  
  if(arg_config._p_motor2Conf)
  {
    arg_config._p_motor2Conf->_itCbs._pfn_hall1It = Hoverboard::motor2Hall1It;
    arg_config._p_motor2Conf->_itCbs._pfn_hall2It = Hoverboard::motor2Hall2It;
    arg_config._p_motor2Conf->_itCbs._pfn_hall3It = Hoverboard::motor2Hall3It;
    arg_config._p_motor2Conf->_itCbs._pfn_timerIt = Hoverboard::motor2TimerIt;
    _hallSensor2 = BrushlessHallSensor(*arg_config._p_motor2Conf);
    _has_motor2HallSensor = true;
  }
  else
  {
    /** Do not use sensor */
    _has_motor2HallSensor = false;
  }
  _instance = this;
}  
  
Hoverboard::EHoverboardErr Hoverboard::init(void)
{
	 _p_gyro1Serial->begin (26300, SERIAL_9N1);  // 9 bits mode
   _p_gyro2Serial->begin (26300, SERIAL_9N1);  // 9 bits mode

  pinMode(_u16_powerPin, OUTPUT);
  pinMode(_u16_powerStatPin, INPUT_PULLUP);
  attachInterrupt(_u16_powerStatPin, Hoverboard::powerOffIt, RISING); 
  
  if(isPowered())
  {
    _e_state = POWER_ON;
    LOG_INFO_LN("startup : POWERING OFF hoverbot"); 
    powerOff(); 
  }
  else
  {
    LOG_INFO_LN("startup : Hoverbot is POWERED OFF"); 
    /** already off */
    _e_state = POWER_OFF;
  }
  
  /** Start hall sensing */
  if(_has_motor1HallSensor)
  {
    _hallSensor1.startSensing();
  }
  if(_has_motor2HallSensor)
  {
    _hallSensor2.startSensing();
  }
  
	return NO_ERROR;
}
  

void Hoverboard::registerListener(HoverboardListener* arg_p_listener)
{
  ASSERT(_p_listener == NULL);
  _p_listener = arg_p_listener;
}

void Hoverboard::unregisterListener(void)
{
  ASSERT(_p_listener != NULL);
  _p_listener = NULL;
}

Hoverboard::EHoverboardErr Hoverboard::powerOn(void)
{
  if(_e_state == POWER_OFF)
  {
    powerOnAsync();
    while(_e_state != POWER_ON)
    {
      /** TODO : IDLE control */
    }
  }
  else
  {
    LOG_DEBUG_LN("already powered on");
  }
  return NO_ERROR;
} 
  
Hoverboard::EHoverboardErr Hoverboard::powerOnAsync(void)
{
    Serial.println("pwoeron"); 
  if(_e_state != POWER_ON || _e_state != POWERING_ON)
  {
    _f_speed1 = 0.0;
    _f_speed2 = 0.0;
    _f_diffSpeed = 0.0;
    _f_commonSpeed = 0.0;
    
    _e_state = POWERING_ON;
    digitalWrite(_u16_powerPin, HIGH);
    _timer.begin(Hoverboard::timerIt, SHORT_PRESS_DUR_MS*1000); 
  }
  return NO_ERROR; 
} 

Hoverboard::EHoverboardErr Hoverboard::powerOff(void)
{
  if(_e_state != POWER_OFF)
  {
    powerOffAsync();
    while(_e_state != POWER_OFF)
    {
      /** TODO : IDLE control */
    }
  } 
  else
  {
    LOG_DEBUG_LN("already powered off");
  }
  return NO_ERROR;
}

Hoverboard::EHoverboardErr Hoverboard::powerOffAsync(void)
{
  if(_e_state != POWER_OFF || _e_state != POWERING_OFF)
  {
    _f_speed1 = 0.0;
    _f_speed2 = 0.0;
    _f_diffSpeed = 0.0;
    _f_commonSpeed = 0.0;
    _e_state = POWERING_OFF;
    digitalWrite(_u16_powerPin, HIGH);
    /** POWER OFF detected on rising edge => increasing press duration makes power off
     * detected later */
    _timer.begin(Hoverboard::timerIt, Hoverboard::SHORT_PRESS_DUR_MS*1000); 
  }
  return NO_ERROR; 
}


bool Hoverboard::isPowered(void)
{
  return !digitalRead(_u16_powerStatPin);
}

Hoverboard::EHoverboardErr Hoverboard::setSpeedAsync(float arg_f_speed1, float arg_f_speed2, uint32_t arg_u32_rampUpDur)
{
  if(_e_state == IDLE)
  {
    setCommonSpeedAsync(arg_f_speed1, arg_f_speed2, arg_u32_rampUpDur); 
    setDifferentialSpeed(arg_f_speed1, arg_f_speed2);
    _f_speed1 = arg_f_speed1; 
    _f_speed2 = arg_f_speed2; 
  }
  else
  {
    LOG_ERROR("setSpeed - bad state %d", _e_state);
  }
} 
    
Hoverboard::EHoverboardErr Hoverboard::stop(void)
{
  
}

void Hoverboard::idleControl(void)
{
  bool loc_b_notify = false;
  float loc_f_newSpeed = 0.0;
  
  if(_e_state == COMMON_SPEED)
  {
    /** Apply common speed during a duration depending on ramp up factor */
    /** Same rota direction => refer README.md - Gyro simualated target must from different
     * signum */
    simulateGyro1(_s16_speedRampUp);
    simulateGyro2(-_s16_speedRampUp);
    
  }
  else if(_e_state != POWER_OFF)
  {
    _e_state = IDLE;
    /** Idle control => apply differential speed (proportional control) */
    simulateGyro1(_f_diffSpeed);
    simulateGyro2(_f_diffSpeed);
  } 
   
  /** Read speed from sensors */ 
  if(_has_motor1HallSensor)
  {
    loc_f_newSpeed =_hallSensor1.getSpeed();
    if(_f_sensorSpeed1 != loc_f_newSpeed)
    { 
      _f_sensorSpeed1 = loc_f_newSpeed;
      loc_b_notify = true;
    } 
  }
  
  if(_has_motor2HallSensor)
  {
     loc_f_newSpeed =_hallSensor2.getSpeed();
    if(_f_sensorSpeed2 != loc_f_newSpeed)
    { 
      _f_sensorSpeed2 = loc_f_newSpeed;
      loc_b_notify = true;
    } 
  }
  
  if(_p_listener && loc_b_notify)
  {
		_p_listener->onSpeedChanged(_f_sensorSpeed1, _f_sensorSpeed2);
	}

  /** Pop pending events (pushed under IT) */
  if(_u32_events)
  {
    if(POWERED_ON_EVENT & _u32_events)
    {
      _u32_events &= ~POWERED_ON_EVENT;
      if(_p_listener)
      {
        _p_listener->onPowerOn(); 
      }
    }

    if(POWERED_OFF_EVENT & _u32_events)
    {
      _u32_events &= ~POWERED_OFF_EVENT;
      if(_p_listener)
      {
        _p_listener->onPowerOff(); 
      }
    }

    if(SPEED_APPLIED_EVENT & _u32_events)
    {
      _u32_events &= ~SPEED_APPLIED_EVENT;
      if(_p_listener)
      {
        _p_listener->onSpeedApplied(); 
      }
    }
  }
}
/**************************************************************************
 * Private Methods Definitions
 **************************************************************************/
Hoverboard::EHoverboardState Hoverboard::getState(void)
{
}

void Hoverboard::simulateGyro1(int16_t arg_s16_target)
{
	/** send a frame simulating gyroscope move on gyro 
	 * daughterboard 1 */
	uint16_t loc_au16_gyroFrame[GYRO_FRAME_LENGTH] = {
		GYRO_FRAME_START, 
		(uint8_t)(arg_s16_target & 0xff), 
		(uint8_t)(arg_s16_target >> 8 & 0xff), 
		(uint8_t)(arg_s16_target & 0xff), 
		(uint8_t)(arg_s16_target >> 8 & 0xff), 
		GYRO_CONTACT_CLOSED_BYTE};

	for(uint8_t index = 0; index < 6; index++)
	{
		_p_gyro1Serial->write9bit(loc_au16_gyroFrame[index]); 
	} 
}  
    
void Hoverboard::simulateGyro2(int16_t arg_s16_target)
{
  /** send a frame simulating gyroscope move on gyro 
   * daughterboard 2 */
  uint16_t loc_au16_gyroFrame[GYRO_FRAME_LENGTH] = {
    GYRO_FRAME_START, 
    (uint8_t)(arg_s16_target & 0xff), 
    (uint8_t)(arg_s16_target >> 8 & 0xff), 
    (uint8_t)(arg_s16_target & 0xff), 
    (uint8_t)(arg_s16_target >> 8 & 0xff), 
    GYRO_CONTACT_CLOSED_BYTE};
  
  for(uint8_t index = 0; index < 6; index++)
  {
    _p_gyro2Serial->write9bit(loc_au16_gyroFrame[index]);
  }
}

/**
* When motors rotate in same direction. Hoverboard performs an integral
* control from daugtherboards gyroscope data. In order to keep a clean 
* control, we must simulate an integral control on simulated gyros/
*/ 
void Hoverboard::setCommonSpeedAsync(float arg_f_speed1, float arg_f_speed2, uint32_t arg_u32_rampUpDur)
{
  float loc_f_commonSpeedDiff = 0.0;
  float loc_f_commonSpeed = 0.0;
  
  /** TODO check min an max values */ 
  ASSERT(arg_u32_rampUpDur > 0); 
  
  _s16_speedRampUp = 0; 

  if(arg_f_speed1 >= 0 && arg_f_speed2 >= 0)
  {
    loc_f_commonSpeed = fmin(arg_f_speed1, arg_f_speed2);
  }
  else if(arg_f_speed1 < 0 && arg_f_speed2 < 0)
  {
    loc_f_commonSpeed = fmax(arg_f_speed1, arg_f_speed2);
  }
  else
  {
    /** No common speed */
  }
  
  loc_f_commonSpeedDiff = loc_f_commonSpeed - _f_commonSpeed;
  if(loc_f_commonSpeedDiff == 0)
  {
    _u32_events |= SPEED_APPLIED_EVENT;
    LOG_DEBUG_LN("Same common speed");
  }
  else
  {
    _s16_speedRampUp =  1000*loc_f_commonSpeedDiff/(float)arg_u32_rampUpDur;      
    
    LOG_DEBUG_LN("Common speed : target(%f %f) actual(%f) new(%f) ramp(%d) duration=%dms",
        arg_f_speed1,
        arg_f_speed2,
        _f_commonSpeed,
        loc_f_commonSpeedDiff,
        _s16_speedRampUp,
        arg_u32_rampUpDur);
    
     
    _f_commonSpeed += loc_f_commonSpeedDiff;
		_e_state = COMMON_SPEED; 
		_timer.begin(Hoverboard::timerIt, arg_u32_rampUpDur*1000); 
  }
} 

/**
* When motors rotate in same direction. Hoverboard performs an integral
* control from daugtherboards gyroscope data. In order to keep a clean 
* control, we must simulate an integral control on simulated gyros.
* Differential control is a proportional control 
*/ 
void Hoverboard::setDifferentialSpeed(float arg_f_speed1, float arg_f_speed2)
{
  if(arg_f_speed1 >=  arg_f_speed2 )
  {
    _f_diffSpeed = (arg_f_speed1 - arg_f_speed2)/2.0; 
  }
  else
  {
    _f_diffSpeed = (arg_f_speed2 - arg_f_speed1)/2.0; 
  }
    
  LOG_DEBUG_LN("diff_speed (%f, %f)", 
     _f_diffSpeed,
     _f_diffSpeed); 
}

void Hoverboard::timerIt(void)
{
  if(_instance->_e_state == POWERING_ON)
  {
    digitalWrite(_instance->_u16_powerPin, LOW);
    if(_instance->isPowered())
    {
      _instance->_e_state = POWER_ON;
      _instance->_u32_events |= POWERED_ON_EVENT;
    }
  }
  else if(_instance->_e_state == POWERING_OFF)
  {
    digitalWrite(_instance->_u16_powerPin, LOW);
    /** Hoverbot not yet stopped - it is stopped on falling edge
     * => POWER OFF will be detected in POWER_OFF it */
  }
  else if(_instance->_e_state == COMMON_SPEED)
  {
    _instance->_e_state = IDLE;
    _instance->_u32_events |= SPEED_APPLIED_EVENT;
  }
  _instance->_timer.end();
}

void Hoverboard::powerOffIt(void)
{
  detachInterrupt(_instance->_u16_powerStatPin); 
  if(!(_instance->isPowered()))
  {
    if(_instance->_e_state == POWERING_OFF)
    {
      _instance->_u32_events |= POWERED_OFF_EVENT;
    } 
    _instance->_e_state = POWER_OFF;
  } 
  attachInterrupt(_instance->_u16_powerStatPin, Hoverboard::powerOffIt, RISING); 
}

/** Motor 1 sensor Cbs */
void Hoverboard::motor1Hall1It(void)
{
  if(_instance->_has_motor1HallSensor)
  {
    _instance->_hallSensor1.hall1It();
  }
  else
  {
    ASSERT(false);
  }
} 

void Hoverboard::motor1Hall2It(void)
{
  if(_instance->_has_motor1HallSensor)
  {
    _instance->_hallSensor1.hall2It();
  }
  else
  {
    ASSERT(false);
  }
}

void Hoverboard::motor1Hall3It(void)
{
  if(_instance->_has_motor1HallSensor)
  {
    _instance->_hallSensor1.hall3It();
  }
  else
  {
    ASSERT(false);
  }
}

void Hoverboard::motor1TimerIt(void)
{
  if(_instance->_has_motor1HallSensor)
  {
    _instance->_hallSensor1.timerIt();
  }
  else
  {
    ASSERT(false);
  }
}

/** Motor 2 sensor Cbs */
void Hoverboard::motor2Hall1It(void)
{
  if(_instance->_has_motor2HallSensor)
  {
    _instance->_hallSensor2.hall1It();
  }
  else
  {
    ASSERT(false);
  }
}

void Hoverboard::motor2Hall2It(void)
{
  if(_instance->_has_motor2HallSensor)
  {
    _instance->_hallSensor2.hall2It();
  }
  else
  {
    ASSERT(false);
  }
}

void Hoverboard::motor2Hall3It(void)
{
  if(_instance->_has_motor2HallSensor)
  {
    _instance->_hallSensor2.hall3It();
  }
  else
  {
    ASSERT(false);
  }
}

void Hoverboard::motor2TimerIt(void)
{
  if(_instance->_has_motor2HallSensor)
  {
    _instance->_hallSensor2.timerIt();
  }
  else
  {
    ASSERT(false);
  }
}


