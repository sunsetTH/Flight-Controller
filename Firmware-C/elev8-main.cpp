/*
  Elev8 Flight Controller
*/

#include <propeller.h>

#include "battery.h"            // Battery monitor functions (charge time to voltage)
#include "beep.h"               // Piezo beeper functions
#include "commlink.h"           // GroundStation communication link
#include "constants.h"          // Project-wide constants, like clock rate, update frequency
#include "elev8-main.h"         // Main thread functions and defines                            (Main thread takes 1 COG)
#include "f32.h"                // 32 bit IEEE floating point math and stream processor         (1 COG)
#include "intpid.h"             // Integer PID functions
#include "pins.h"               // Pin assignments for the hardware
#include "prefs.h"              // User preferences storage
#include "quatimu.h"            // Quaternion IMU and control functions
#include "rc.h"                 // High precision 8-port R/C PWM input driver                   (1 COG, if enabled)
#include "sbus.h"               // S-BUS (Futaba 1-wire receiver) driver                        (1 COG, if enabled)
#include "sensors.h"            // Sensors (gyro,accel,mag,baro) + LEDs driver                  (1 COG)
#include "serial_4x.h"          // 4 port simultaneous serial I/O                               (1 COG)
#include "servo32_highres.h"    // 32 port, high precision / high rate PWM servo output driver  (1 COG)


// TODO: Might want to consider accelerating the comms by caching the buffer pointers, etc.

COMMLINK Comm;          // Communication protocol for Elev8

short UsbPulse = 0;     // Periodically, the GroundStation will ping the FC to say it's still there - these are countdowns
short XBeePulse = 0;


// Potential new settings values
const int AltiThrottleDeadband = 100;


//Working variables for the flight controller

//Receiver inputs
static RADIO Radio;
static long  iRudd;         //Integrated rudder input value
static long  LoopCycles = 0;
//Sensor inputs, in order of outputs from the Sensors cog, so they can be bulk copied for speed
static SENS sens;


static long  GyroZX, GyroZY, GyroZZ;  // Gyro zero values

static long  AccelZSmooth;            // Smoothed (filtered) accelerometer Z value (used for height fluctuation damping)

//Debug output mode, working variables  
static long   counter;        //Main loop iteration counter
static short  TxData[12];     //Word-sized copies of Temp, Gyro, Accel, Mag, for debug transfer speed
static char   Quat[16];       //Current quaternion from the IMU functions

static char Mode;                     //Debug communication mode
static signed char NudgeMotor;        // Which motor to nudge during testing (-1 == no motor)

static char AccelAssistZFactor  = 32; // 0 to 64 == 0 to 1.0

static long  AltiEst, AscentEst;                              // altitude estimate and ascent rate estimate
static long  DesiredAltitude, DesiredAscentRate;              // desired values for altitude and ascent rate

static long  RollDifference, PitchDifference, YawDifference;  //Delta between current measured roll/pitch and desired roll/pitch                         
static long  GyroRoll, GyroPitch, GyroYaw;                    //Raw gyro values altered by desired pitch & roll targets

static long  GyroRPFilter, GyroYawFilter;   // Tunable damping values for gyro noise


static long  Motor[4];                      //Motor output values  
static long  LEDValue[LED_COUNT];           //LED outputs (copied to the LEDs by the Sensors cog)

static long loopTimer;                      //Master flight loop counter - used to keep a steady update rate

static long PingZero, PingHeight;

static short FlightEnableStep;        //Flight arm/disarm counter
static short CompassConfigStep;       //Compass configure mode counter

static char FlightEnabled;            //Flight arm/disarm flag
static char FlightMode;
static char IsHolding;                // Are we currently in altitude hold? (hover mode)

static short BatteryMonitorDelay;     //Can't start discharging the battery monitor cap until the ESCs are armed or the noise messes them up

static char MotorPin[4];            //Motor index to pin index table


static long RollPitch_P, RollPitch_D; //Tunable values for roll & pitch Proportional gain and Derivative gain
static long LEDModeColor;

static short BatteryVolts = 0;


static char calib_startQuadrant;
static char calib_Quadrants;
static char calib_step;
static long c_xmin, c_ymin, c_xmax, c_ymax, c_zmin, c_zmax;

// PIDs for roll, pitch, yaw, altitude
static IntPID  RollPID, PitchPID, YawPID, AltPID, AscentPID;


// Used to attenuate the brightness of the LEDs, if desired.  A shift of zero is full brightness
const int LEDBrightShift = 0;
const int LEDSingleMask = 0xFF - ((1<<LEDBrightShift)-1);
const int LEDBrightMask = LEDSingleMask | (LEDSingleMask << 8) | (LEDSingleMask << 16);


/*  // Only used for debugging
static void TxInt( int x , int Digits )
{
  static char nybbles[] = "0123456789ABCDEF";
  int shift = 32 - ((9-Digits)<<2);
  do {
    Tx( nybbles[ (x>>shift) & 0xf] );
    shift -= 4;
  } while( shift >= 0 );
}
*/


int main()                                    // Main function
{
  Initialize(); // Set up all the objects
  
  //Prefs_Test();

  //Grab the first set of sensor readings (should be ready by now)
  memcpy( &sens, Sensors_Address(), Sensors_ParamsSize );

  //Set a reasonable starting point for the altitude computation
  QuatIMU_SetInitialAltitudeGuess( sens.Alt );

  Mode = MODE_None;
  counter = 0;
  NudgeMotor = -1;
  loopTimer = CNT;

  // Set all the motors to their low-throttle point
  for( int i=0; i<4; i++ ) {
    Motor[i] = Prefs.MinThrottle;
    Servo32_Set( MotorPin[i], Prefs.MinThrottle );
  }


  while(1)
  {
    int Cycles = CNT;

    //Read ALL inputs from the sensors into local memory, starting at Temperature
    memcpy( &sens, Sensors_Address(), Sensors_ParamsSize );

    QuatIMU_Update( (int*)&sens.GyroX );        //Entire IMU takes ~125000 cycles
    AccelZSmooth += (sens.AccelZ - AccelZSmooth) * Prefs.AccelCorrectionFilter / 256;

    if( Prefs.UseSBUS )
    {
      // Unrolling these loops saves about 10000 cycles, but costs a little over 1/2kb of code space
      for( int i=0; i<8; i++ ) {
        Radio.Channel(i) =  (SBUS::GetRC(Prefs.ChannelIndex(i)) - Prefs.ChannelCenter(i)) * Prefs.ChannelScale(i) / 1024;
      }
    }
    else
    {
      for( int i=0; i<8; i++ ) {
        Radio.Channel(i) =  (RC::GetRC( Prefs.ChannelIndex(i)) - Prefs.ChannelCenter(i)) * Prefs.ChannelScale(i) / 1024;
      }        
    }

    
    //if( UsePing )
    //  PingCycle := counter & 15
    //  if( PingCycle == 0 )
    //    Ping.Fire( PIN#PING )
    //  elseif( PingCycle == 15 )
    //    PingHeight := Ping.Millimeters( PIN#PING ) - PingZero 

      //-------------------------------------------------
    if( FlightMode == FlightMode_CalibrateCompass )
    {
      DoCompassCalibrate();
    }
    //-------------------------------------------------
    else
    //-------------------------------------------------
    {
      char NewFlightMode;

      if( Radio.Gear > 512 )
        NewFlightMode = FlightMode_Assisted;
      else if( Radio.Gear < -512 )
        NewFlightMode = FlightMode_Manual;
      else
        NewFlightMode = FlightMode_Automatic;


      if( NewFlightMode != FlightMode )
      {
        if( NewFlightMode == FlightMode_Manual ) {
          QuatIMU_ResetDesiredOrientation();
        }
        else {
          QuatIMU_ResetDesiredYaw();          // Sync the heading when switching from manual to auto-level
        }

        if( NewFlightMode == FlightMode_Automatic ) {
          DesiredAltitude = AltiEst;
        }

        // ANY flight mode change means you're not currently holding altitude
        IsHolding = 0;

        FlightMode = NewFlightMode;
      }

      UpdateFlightLoop();            //~72000 cycles when in flight mode
      //-------------------------------------------------
    }  


    if( Prefs.UseBattMon )
    {
      if( BatteryMonitorDelay > 0 ) {
        BatteryMonitorDelay--;  // Count down until the startup delay has passed
        LEDModeColor = LED_Blue;
      }
      else
      {
        // Update the battery voltage
        switch( counter & 15 )
        {
          case 0: Battery::DischargePin();   break;
          case 2: Battery::ChargePin();      break;
    
          case 15:
            BatteryVolts = Battery::ComputeVoltage( Battery::ReadResult() ) + Prefs.VoltageOffset;
            break;
        }      
      }
    }

    All_LED( LEDModeColor );
    QuatIMU_WaitForCompletion();    // Wait for the IMU to finish updating


    QuatIMU_UpdateControls( &Radio , FlightMode == FlightMode_Manual );   // Now update the control quaternion
    QuatIMU_WaitForCompletion();

    PitchDifference = QuatIMU_GetPitchDifference();
    RollDifference = QuatIMU_GetRollDifference();
    YawDifference = -QuatIMU_GetYawDifference();


    AltiEst = QuatIMU_GetAltitudeEstimate();
    AscentEst = QuatIMU_GetVerticalVelocityEstimate();

    CheckDebugInput();
    DoDebugModeOutput();

    LoopCycles = CNT - Cycles;    // Record how long it took for one full iteration

    ++counter;
    loopTimer += Const_UpdateCycles;
    waitcnt( loopTimer );
  }
}


void Initialize(void)
{
  //Initialize everything - First reset all variables to known states

  MotorPin[0] = PIN_MOTOR_FL;
  MotorPin[1] = PIN_MOTOR_FR;
  MotorPin[2] = PIN_MOTOR_BR;
  MotorPin[3] = PIN_MOTOR_BL;

  Mode = MODE_None;
  counter = 0;
  NudgeMotor = -1;                                      //No motor to nudge
  FlightEnabled = 0; 

  FlightEnableStep = 0;                                 //Counter to know which section of enable/disable sequence we're in
  CompassConfigStep = 0;
  FlightMode = FlightMode_Assisted;
  GyroRPFilter = 192;                                   //Tunable damping filters for gyro noise, 1 (HEAVY) to 256 (NO) filtering 
  GyroYawFilter = 192;

  InitSerial();

  //dbg = fdserial_open( 31, 30, 0, 115200 );   // USB
  //dbg = fdserial_open( 24, 25, 0, 57600 );     // XBee - V2 board (packets contain dropped bytes, likely need to reduce rate, add checksum)

  All_LED( LED_Red & LED_Half );                         //LED red on startup

  // Do this before settings are loaded, because Sensors_Start resets the drift coefficients to defaults
  Sensors_Start( PIN_SDI, PIN_SDO, PIN_SCL, PIN_CS_AG, PIN_CS_M, PIN_CS_ALT, PIN_LED, (int)&LEDValue[0], LED_COUNT );

  F32::Start();
  QuatIMU_Start();

  InitializePrefs();

  if( Prefs.UseSBUS ) {
    SBUS::Start( PIN_RC_0 ); // Doesn't matter - it'll be compensated for by the channel scaling/offset code
  }
  else {
    RC::Start();
  }

  // Wait 2 seconds after startup to begin checking battery voltage, rounded to an integer multiple of 16 updates
  BatteryMonitorDelay = (Const_UpdateRate * 2) & ~15;

#ifdef __PINS_V3_H__
  Battery::Init( PIN_VBATT );
#endif

  DIRA |= (1<<PIN_BUZZER_1) | (1<<PIN_BUZZER_2);      //Enable buzzer pins
  OUTA &= ~((1<<PIN_BUZZER_1) | (1<<PIN_BUZZER_2));   //Set the pins low


  Servo32_Init( 400 );
  for( int i=0; i<4; i++ ) {
    Servo32_AddFastPin( MotorPin[i] );
    Servo32_Set( MotorPin[i], Prefs.MinThrottle );
  }    
  Servo32_Start();

  RollPitch_P = 8000;           //Set here to allow an in-flight tuning baseline
  RollPitch_D = 20000 * Const_UpdateRate;


  RollPID.Init( RollPitch_P, 0,  RollPitch_D , Const_UpdateRate );
  RollPID.SetPrecision( 12 );
  RollPID.SetMaxOutput( 3000 );
  RollPID.SetPIMax( 100 );
  RollPID.SetMaxIntegral( 1900 );
  RollPID.SetDervativeFilter( 128 );    // was 96


  PitchPID.Init( RollPitch_P, 0,  RollPitch_D , Const_UpdateRate );
  PitchPID.SetPrecision( 12 );
  PitchPID.SetMaxOutput( 3000 );
  PitchPID.SetPIMax( 100 );
  PitchPID.SetMaxIntegral( 1900 );
  PitchPID.SetDervativeFilter( 128 );


  YawPID.Init( 15000,  200 * Const_UpdateRate,  10000 * Const_UpdateRate , Const_UpdateRate );
  YawPID.SetPrecision( 12 );
  YawPID.SetMaxOutput( 5000 );
  YawPID.SetPIMax( 100 );
  YawPID.SetMaxIntegral( 2000 );
  YawPID.SetDervativeFilter( 192 );


  // TODO: Need to get an XBee or a logger going here, 'cause I need to see what's happening inside these objects
  // data logs would be very helpful for tuning these.  The cascaded controllers are kind of magically weird.

 
  // Altitude hold PID object
  // The altitude hold PID object feeds speeds into the vertical rate PID object, when in "hold" mode
  AltPID.Init( 600, 500 * Const_UpdateRate, 0, Const_UpdateRate );
  AltPID.SetPrecision( 14 );
  AltPID.SetMaxOutput( 5000 );    // Fastest the altitude hold object will ask for is 5000 mm/sec (5 M/sec)
  AltPID.SetPIMax( 1000 );
  AltPID.SetMaxIntegral( 4000 );


  // Vertical rate PID object
  // The vertical rate PID object manages vertical speed in alt hold mode
  AscentPID.Init( 1100, 0 * Const_UpdateRate, 0 * Const_UpdateRate , Const_UpdateRate );
  AscentPID.SetPrecision( 12 );
  AscentPID.SetMaxOutput( 4000 );   // Limit of the control rate applied to the throttle
  AscentPID.SetPIMax( 500 );
  AscentPID.SetMaxIntegral( 2000 );

  
  FindGyroZero();
}

static char RXBuf1[32], TXBuf1[64];
static char RXBuf2[32], TXBuf2[64];
static char RXBuf3[128],TXBuf3[4];  // GPS?
static char RXBuf4[4],  TXBuf4[4];  // Unused

void InitSerial(void)
{
  S4_Initialize();

  S4_Define_Port(0, 115200,      30, TXBuf1, sizeof(TXBuf1),      31, RXBuf1, sizeof(RXBuf1));
  S4_Define_Port(1,  57600, XBEE_TX, TXBuf2, sizeof(TXBuf2), XBEE_RX, RXBuf2, sizeof(RXBuf2));

  // Unused ports get a pin value of 32
  S4_Define_Port(2,   9600, 32, TXBuf3, sizeof(TXBuf3), 32, RXBuf3, sizeof(RXBuf3));
  S4_Define_Port(3,   2400, 32, TXBuf4, sizeof(TXBuf4), 32, RXBuf4, sizeof(RXBuf4));

  S4_Start();
}


static int clamp( int v, int min, int max ) {
  v = (v < min) ? min : v;
  v = (v > max) ? max : v;
  return v;
}

static int abs(int v) {
  v = (v<0) ? -v : v;
  return v;
}

static int max( int a, int b ) { return a > b ? a : b; }
static int min( int a, int b ) { return a < b ? a : b; }



void FindGyroZero(void)
{
  // The idea here is that it's VERY hard for someone to hold a thing perfectly still.
  // If we detect variation in the gyro, keep waiting until it settles down, or "too long" has elapsed.

  int vmin[3], vmax[3], avg[3];     // min, max, avg readings for each gyro axis
  int best[3], bestvar = -1;        // best set of readings found so far, and the variance for them

  int TryCounter = 0;
  const int MinTries = 2, MaxTries = 64;

  // Wait for any buzzer vibration to stop.  Yes, this is actually necessary, it can be that sensitive.
  waitcnt( CNT + Const_ClockFreq/50 );

  do {
    // Take an initial sensor reading for each axis as a starting point, and zero the average
    for( int a=0; a<3; a++) {
      vmin[a] = vmax[a] = Sensors_In(1+a);
      avg[a] = 0;
    }      

    // take a bunch of readings over about 1/10th of a second, keeping track of the min, max, and sum (average)
    for( int i=0; i<64; i++ )
    {
      for( int a=0; a<3; a++) {
        int v = Sensors_In(1+a);
        vmin[a] = min(vmin[a], v);
        vmax[a] = max(vmax[a], v);
        avg[a] += v;
      }

      waitcnt( CNT + Const_ClockFreq/500 );
    }

    // Compute the mid-point between the min & max, and how different that is from the average (variation)
    int maxVar = 0;
    for( int a=0; a<3; a++)
    {
      avg[a] /= 64;

      // range is the difference between min and max over the sample period.
      // I measured this as ~15 units on all axis when totally still
      int range = vmax[a] - vmin[a];

      // variation is how centered the average is between the min and max.
      // if the craft is perfectly still, this *should* be zero or VERY close.
      int var = (vmax[a]+vmin[a])/2 - avg[a];

      maxVar = max( maxVar, abs(var) );
    }

    if( (bestvar == -1) || (maxVar < bestvar) ) {
      best[0] = avg[0];
      best[1] = avg[1];
      best[2] = avg[2];
      bestvar = maxVar;
    }

    // Every 4th loop, beep at the user to tell them what's happening
    if( (TryCounter & 3) == 3 ) {
      BeepHz( 4000, 80 );
    }

    TryCounter++;

    // Run at least MinTries iterations, wait until max variance is 2 or less, give up after MaxTries
  } while( TryCounter < MaxTries && (bestvar > 2 || TryCounter < MinTries) );

  GyroZX = best[0];
  GyroZY = best[1];
  GyroZZ = best[2];

  QuatIMU_SetGyroZero( GyroZX, GyroZY, GyroZZ );
}


void UpdateFlightLoop(void)
{
  int ThroOut, T1, T2, ThrustMul, AltiThrust, v, gr, gp, gy;
  char DoIntegrate;  //Integration enabled in the flight PIDs?

  UpdateFlightLEDColor();

  //Test for flight mode change-----------------------------------------------
  if( FlightEnabled == 0 )
  {
    //Are the sticks being pushed down and toward the center?

    if( (Radio.Thro < -750)  &&  (Radio.Elev < -750) )
    {
      if( (Radio.Rudd > 750)  &&  (Radio.Aile < -750) )
      {
        FlightEnableStep++;
        CompassConfigStep = 0;
        LEDModeColor = LED_Yellow & LED_Half;
                
        if( FlightEnableStep >= Prefs.ArmDelay ) {   //Hold for delay time
          ArmFlightMode();
        }          
      }
      else if( (Radio.Rudd > 750)  &&  (Radio.Aile > 750) )
      {
        CompassConfigStep++;
        FlightEnableStep = 0;

        LEDModeColor = (LED_Blue | LED_Red) & LED_Half;
                
        if( CompassConfigStep == 250 ) {   //Hold for 1 second
          StartCompassCalibrate();
        }
      }
      else
      {
        CompassConfigStep = 0;
        FlightEnableStep = 0;
      }
    }
    else
    {
      CompassConfigStep = 0;
      FlightEnableStep = 0;
    }
    //------------------------------------------------------------------------
  }       
  else
  {
    //Are the sticks being pushed down and away from center?

    if( (Radio.Rudd < -750)  &&  (Radio.Aile > 750)  &&  (Radio.Thro < -750)  &&  (Radio.Elev < -750) )
    {
      FlightEnableStep++;
      LEDModeColor = LED_Yellow & LED_Half;

      if( FlightEnableStep >= Prefs.DisarmDelay ) {   //Hold for delay time
        DisarmFlightMode();
        return;                  //Prevents the motor outputs from being un-zero'd
      }        
    }      
    else {
      FlightEnableStep = 0;
    }
    //------------------------------------------------------------------------


    gr =  sens.GyroY - GyroZY;
    gp = -(sens.GyroX - GyroZX);
    gy = -(sens.GyroZ - GyroZZ);

    GyroRoll += ((gr - GyroRoll) * GyroRPFilter) >> 8;
    GyroPitch += ((gp - GyroPitch) * GyroRPFilter) >> 8;
    GyroYaw += ((gy - GyroYaw) * GyroYawFilter) >> 8;



    if( Radio.Thro < -800 )
    {
      // When throttle is essentially zero, disable all control authority

      if( FlightMode == FlightMode_Manual ) {
        QuatIMU_ResetDesiredOrientation();
      }
      else {
        // Zero yaw target when throttle is off - makes for more stable liftoff
        QuatIMU_ResetDesiredYaw();
      }

      DoIntegrate = 0;          // Disable PID integral terms until throttle is applied      
    }      
    else {
      DoIntegrate = 1;
    }


    int RollOut = RollPID.Calculate( RollDifference , GyroRoll , DoIntegrate );
    int PitchOut = PitchPID.Calculate( PitchDifference , GyroPitch , DoIntegrate );
    int YawOut = YawPID.Calculate( YawDifference, GyroYaw, DoIntegrate );


    int ThroMix = (Radio.Thro + 1024) >> 2;           // Approx 0 - 512
    ThroMix = clamp( ThroMix, 0, 64 );                // Above 1/8 throttle, clamp it to 64
     
    //add 12000 to all Output values to make them 'servo friendly' again   (12000 is our output center)
    ThroOut = (Radio.Thro << 2) + 12000;

    //-------------------------------------------
    if( FlightMode != FlightMode_Manual )
    {
      if( FlightMode == FlightMode_Automatic )
      {
        int AdjustedThrottle = 0;

        // Throttle has to be off zero by a bit - deadband around zero helps keep it still
        if( abs(Radio.Thro) > AltiThrottleDeadband )
        {
          IsHolding = 0;    // No longer attempting to hold a hover

          // Remove the deadband area from center stick so we don't get a hiccup as you transition out of it
          AdjustedThrottle = (Radio.Thro > 0) ? (Radio.Thro - AltiThrottleDeadband) : (Radio.Thro + AltiThrottleDeadband);

          DesiredAscentRate = AdjustedThrottle * 6000 / (1024 - AltiThrottleDeadband);
        }
        else
        {
          // Are we just entering altitude hold mode?
          if( IsHolding == 0 ) {
            IsHolding = 1;
            DesiredAltitude = AltiEst;  // Start with our current altitude as the hold height
            AltPID.ResetIntegralError();
          }

          // in-flight PID tuning
          //T1 = 1024 + Radio.Aux2;             // Left side control
          //T2 = 1250 + Radio.Aux3;             // Right side control

          //AltPID.SetPGain( T1 );
          //AltPID.SetIGain( T2 * 250 );


          // Use a PID object to compute velocity requirements for the AscentPID object
          DesiredAscentRate = AltPID.Calculate( DesiredAltitude , AltiEst, DoIntegrate );
        }


        // in-flight PID tuning
        T1 = 1024 + Radio.Aux2;             // Left side control
        T2 = 1024 + Radio.Aux3;             // Right side control

        AscentPID.SetPGain( T1 );
        AscentPID.SetIGain( T2 * 250 );


        AltiThrust = AscentPID.Calculate( DesiredAscentRate , AscentEst , DoIntegrate );
        ThroOut = Prefs.CenterThrottle + AltiThrust + AdjustedThrottle; // Feed in a bit of the user throttle to help with quick throttle changes
      }

      if( AccelAssistZFactor > 0 )
      {
        // Accelerometer assist    
        if( abs(Radio.Aile) < 300 && abs(Radio.Elev) < 300 && ThroMix > 32) { //Above 1/8 throttle, add a little AccelZ into the mix if the user is trying to hover
          ThroOut -= ((AccelZSmooth - Const_OneG) * (int)AccelAssistZFactor) / 64;
        }
      }

      if( Prefs.ThrustCorrectionScale > 0 )
      {
        // Tilt compensated thrust assist
        ThrustMul = 256 + ((QuatIMU_GetThrustFactor() - 256) * Prefs.ThrustCorrectionScale) / 256;
        ThrustMul = clamp( ThrustMul, 256 , 384 );    //Limit the effect of the thrust modifier
        ThroOut = Prefs.MinThrottle + (((ThroOut-Prefs.MinThrottle) * ThrustMul) >> 8);
      }        
    }
    //-------------------------------------------


    //X configuration
    Motor[OUT_FL] = ThroOut + (((+PitchOut + RollOut - YawOut) * ThroMix) >> 7);
    Motor[OUT_FR] = ThroOut + (((+PitchOut - RollOut + YawOut) * ThroMix) >> 7);
    Motor[OUT_BL] = ThroOut + (((-PitchOut + RollOut + YawOut) * ThroMix) >> 7);
    Motor[OUT_BR] = ThroOut + (((-PitchOut - RollOut - YawOut) * ThroMix) >> 7);


    // The low-throttle clamp prevents combined PID output from sending the ESCs below a minimum value
    // Some ESCs appear to stall (go into "stop" mode) if the throttle gets too close to zero, even for a moment, so avoid that
     
    Motor[0] = clamp( Motor[0], Prefs.MinThrottleArmed , Prefs.MaxThrottle);
    Motor[1] = clamp( Motor[1], Prefs.MinThrottleArmed , Prefs.MaxThrottle);
    Motor[2] = clamp( Motor[2], Prefs.MinThrottleArmed , Prefs.MaxThrottle);
    Motor[3] = clamp( Motor[3], Prefs.MinThrottleArmed , Prefs.MaxThrottle);

    if( Prefs.DisableMotors == 0 ) {
      //Copy new Ouput array into servo values
      Servo32_Set( PIN_MOTOR_FL, Motor[0] );
      Servo32_Set( PIN_MOTOR_FR, Motor[1] );
      Servo32_Set( PIN_MOTOR_BR, Motor[2] );
      Servo32_Set( PIN_MOTOR_BL, Motor[3] );
    }
  }

#if defined( __PINS_V3_H__ )
    // Battery alarm at low voltage
    if( Prefs.UseBattMon != 0 && Prefs.LowVoltageAlarm != 0 )
    {
      // If we want to use the PING sensor *and* use a timer for the alarm, we'll need to
      // move the freq generator onto another cog.  Currently the battery monitor uses CTRB
      // to count charge time.  Ideally the PING sensor would use CTRA to count return time,
      // so we can have one or the other in the main thread, but not both.

      if( (BatteryVolts < Prefs.LowVoltageAlarmThreshold) && (BatteryVolts > 200) && ((counter & 63) == 0) )  // Make sure the voltage is above the (0 + VoltageOffset) range
      {
        BeepOn( 'A' , PIN_BUZZER_1, 5000 );
      }
      else if( (counter & 63) > 32 )
      {
        BeepOff( 'A' );
      }        
    }      
#endif
}


static int LEDColorTable[] = {
        /* LED_Assisted  */    LED_Cyan,
        /* LED_Automatic */    LED_White,
        /* LED_Manual    */    LED_Yellow,
        /* LED_CompCalib */    LED_Violet,
};

static int LEDArmDisarm[] = {
        /* LED_Disarmed */     LED_Green,
        /* LED_Armed    */     LED_Red,
};


void UpdateFlightLEDColor(void)
{
  int LowBatt = 0;

#if defined( __PINS_V3_H__ )
  if( Prefs.UseBattMon != 0 && (BatteryVolts < Prefs.LowVoltageAlarmThreshold) && BatteryVolts > 200 ) {   // Make sure the voltage is above the (0 + VoltageOffset) range
    LowBatt = 1;
  }    
#endif

  if( LowBatt ) {

    int index = (counter >> 3) & 7;

    if( index < 4 ) {
      LEDModeColor = (LEDColorTable[FlightMode & 3] & LEDBrightMask) >> LEDBrightShift;
    }
    else {
      LEDModeColor = ((LED_Red | (LED_Yellow & LED_Half)) & LEDBrightMask) >> LEDBrightShift;   // Fast flash orange for battery warning
    }

  }
  else {
    int index = (counter >> 3) & 15;
    if( index < 3 || IsHolding ) {  // Temporary, so I can tell
      LEDModeColor = (LEDColorTable[FlightMode & 3] & LEDBrightMask) >> LEDBrightShift;
    }    
    else {
      LEDModeColor = (LEDArmDisarm[FlightEnabled & 1] & LEDBrightMask) >> LEDBrightShift;
    }
  }
}


void ArmFlightMode(void)
{
  FlightEnabled = 1;
  FlightEnableStep = 0;
  CompassConfigStep = 0;
  Beep2();
   
  All_LED( LED_Red & LED_Half );
  FindGyroZero();
   
  All_LED( LED_Blue & LED_Half );
  BeepTune();

  DesiredAltitude = AltiEst;
  loopTimer = CNT;
}

void DisarmFlightMode(void)
{
  for( int i=0; i<4; i++ ) {
    Motor[i] = Prefs.MinThrottle;
    Servo32_Set( MotorPin[i], Prefs.MinThrottle );
  }

  FlightEnabled = 0;
  FlightEnableStep = 0;
  CompassConfigStep = 0;
  Beep3();
  
  All_LED( LED_Green & LED_Half );
  loopTimer = CNT;
}

void StartCompassCalibrate(void)
{
  // Placeholder
}  

void DoCompassCalibrate(void)
{
  // Placeholder
}


void CheckDebugInput(void)
{
  int i;

  char port = 0;
  int c = S4_Check(0);
  if(c < 0) {
    c = S4_Check(1);
    if(c < 0)
      return;
    port = 1;
  }

  if( c <= MODE_MotorTest ) {
    Mode = c;
    if( port == 0 ) {
      UsbPulse = 500;   // send USB data for the next two seconds (we'll get another heartbeat before then)
      XBeePulse = 0;
    }      
    if( port == 1 ) {
      UsbPulse = 0;
      XBeePulse = 500;  // send XBee data for the next two seconds (we'll get another heartbeat before then)
    }      
    return;
  }

  if( port == 0 && (c & 0xF8) == 0x08 ) {  //Nudge one of the motors (ONLY allowed over USB)
      NudgeMotor = c & 7;  //Nudge the motor selected by the configuration app
      return;
  }

  if( (c & 0xF8) == 0x10 )  //Zero, Reset, or set gyro or accelerometer calibration
  {
    if( Mode == MODE_SensorTest )
    {
      if( c == 0x10 )
      {
        //Temporarily zero gyro drift settings
        Sensors_TempZeroDriftValues();
      }
      else if( c == 0x11 )
      {
        //Reset gyro drift settings
        Sensors_ResetDriftValues();
      }
      else if( c == 0x13 )
      {
        for( int i=0; i<8; i++ ) {
          Prefs.ChannelScale(i) = 1024;
          Prefs.ChannelCenter(i) = 0;
        }

        Beep2();
        loopTimer = CNT;
      }
      else if( c == 0x14 )
      {
        //Temporarily zero accel offset settings
        Sensors_TempZeroAccelOffsetValues();
      }
      else if( c == 0x15 )
      {
        //Reset accel offset settings
        Sensors_ResetAccelOffsetValues();
      }

    }      
  }


  if( (c & 0xf8) == 0x18 )        //Query or modify all settings
  {
    if( c == 0x18 )               //Query current settings
    {
      Prefs.Checksum = Prefs_CalculateChecksum( Prefs );
      int size = sizeof(Prefs);

      Comm.StartPacket( 0, 0x18 , size );
      Comm.AddPacketData( 0, &Prefs, size );
      Comm.EndPacket(port);

      loopTimer = CNT;                                                          //Reset the loop counter in case we took too long 
    }
    else if( c == 0x19 )          //Store new settings
    {
      PREFS TempPrefs;
      for( int i=0; i<sizeof(Prefs); i++ ) {
        ((char *)&TempPrefs)[i] = S4_Get_Timed(port, 50);   // wait up to 50ms per byte - Should be plenty
      }

      if( Prefs_CalculateChecksum( TempPrefs ) == TempPrefs.Checksum ) {
        memcpy( &Prefs, &TempPrefs, sizeof(Prefs) );
        Prefs_Save();

        if( Prefs_Load() ) {
          BeepOff( 'A' );   // turn off the alarm beeper if it was on
          Beep2();
          ApplyPrefs();
        }
        else {
          Beep();
        }
      }
      else {
        Beep();
      }        
      loopTimer = CNT;                                                          //Reset the loop counter in case we took too long 
    }
    else if( c == 0x1a )    // default prefs - wipe
    {
      if( S4_Get_Timed(port, 50) == 0x1a )
      {
        Prefs_SetDefaults();
        Prefs_Save();
        Beep3();
      }
      loopTimer = CNT;                                                          //Reset the loop counter in case we took too long 
    }
  }

  if( c == 0xff ) {
    S4_Put(port, 0xE8);     //Simple ping-back to tell the application we have the right comm port
  }
}

void DoDebugModeOutput(void)
{
  int loop, addr, i, phase;
  char port = 0;

  if( UsbPulse > 0 ) {
    if( --UsbPulse == 0 ) {
      Mode = MODE_None;
      return;
    }
    phase = counter & 7;    // Translates to 31.25 full updates per second, at 250hz
  }
  else if( XBeePulse > 0 )
  {
    if( --UsbPulse == 0 ) {
      Mode = MODE_None;
      return;
    }
    port = 1;
    phase = ((counter >> 1) & 7) | ((counter & 1) << 16);    // Translates to ~15 full updates per second, at 250hz
  }    

  if( Mode == MODE_None ) return;

  switch( Mode )
  {
    case MODE_SensorTest:
    {
      switch( phase )
      {
      case 0:
        Comm.StartPacket( 1, 18 );               // Radio values, 22 byte payload
        Comm.AddPacketData( &Radio , 16 );       // Radio struct is 16 bytes total
        Comm.AddPacketData( &BatteryVolts, 2 );  // Send 2 additional bytes for battery voltage
        Comm.EndPacket();
        Comm.SendPacket(port);
        break;

      case 1:
        Comm.StartPacket( 7, 8 );                 // Debug values, 8 byte payload
        Comm.AddPacketData( &LoopCycles, 4 );     // Cycles required for one complete loop  (debug data takes a LONG time)

        QuatIMU_GetDebugFloat( (float*)TxData );  // This is just a debug value, used for testing outputs with the IMU running
        Comm.AddPacketData( TxData, 4 );
        Comm.EndPacket();
        Comm.SendPacket(port);
        break;

      case 2:
        TxData[0] = sens.Temperature;       //Copy the values we're interested in into a WORD array, for faster transmission                        
        TxData[1] = sens.GyroX;
        TxData[2] = sens.GyroY;
        TxData[3] = sens.GyroZ;
        TxData[4] = sens.AccelX;
        TxData[5] = sens.AccelY;
        TxData[6] = sens.AccelZ;
        TxData[7] = sens.MagX;
        TxData[8] = sens.MagY;
        TxData[9] = sens.MagZ;

        Comm.BuildPacket( 2, &TxData, 20 );   //Send 22 bytes of data from @TxData onward (sends 11 words worth of data)
        Comm.SendPacket(port);
        break;

      case 4:
        Comm.BuildPacket( 3, QuatIMU_GetQuaternion(), 16 );  // Quaternion data, 16 byte payload
        Comm.SendPacket(port);
        break;

      case 5: // Motor data
        TxData[0] = Motor[0];
        TxData[1] = Motor[1];
        TxData[2] = Motor[2];
        TxData[3] = Motor[3];
        Comm.BuildPacket( 5, &TxData, 8 );   // 8 byte payload
        Comm.SendPacket(port);
        break;


      case 6:
        Comm.StartPacket( 4, 24 );   // Computed data, 24 byte payload

        Comm.AddPacketData( &PitchDifference, 4 );
        Comm.AddPacketData( &RollDifference, 4 );
        Comm.AddPacketData( &YawDifference, 4 );

        Comm.AddPacketData( &sens.Alt, 4 );       //Send 4 bytes of data for @Alt
        Comm.AddPacketData( &sens.AltTemp, 4 );   //Send 4 bytes of data for @AltTemp
        Comm.AddPacketData( &AltiEst, 4 );        //Send 4 bytes for altitude estimate 
        Comm.EndPacket();
        Comm.SendPacket(port);
        break;

      case 7:
        QuatIMU_GetDesiredQ( (float*)TxData );
        Comm.BuildPacket( 6, TxData, 16 );   // Desired Quaternion data, 16 byte payload
        Comm.SendPacket(port);
        break;
      }
    }
    break;
  }


  //Motor test code---------------------------------------
  if( NudgeMotor > -1 )
  {
    if( NudgeMotor < 4 )
    {
      Servo32_Set(MotorPin[NudgeMotor], 9500);       //Motor test - 1/8 throttle - TODO - Make this use Prefs.TestThrottle value, or calc 1/8th from range
    }
    else if( NudgeMotor == 4 )                         //Buzzer test
    {
      BeepHz(4500, 50);
      waitcnt( CNT + 5000000 );
      BeepHz(3500, 50);
    }      
    else if( NudgeMotor == 5 )                         //LED test
    {
      //RGB led will run a rainbow
      for( i=0; i<256; i++ ) {
        All_LED( ((255-i)<<16) + (i<<8) );
        waitcnt( CNT + 160000 );
      }          

      for( i=0; i<256; i++ ) {
        All_LED( i + ((255-i) << 8) );
        waitcnt( CNT + 160000 );
      }          

      for( i=0; i<256; i++ ) {
        All_LED( (255-i) + (i<<16) );
        waitcnt( CNT + 160000 );
      }          
    }
    else if( NudgeMotor == 6 )                         //ESC Throttle calibration
    {
      BeepHz(4500, 100);
      waitcnt( CNT + 5000000 );
      BeepHz(4500, 100);
      waitcnt( CNT + 5000000 );
      BeepHz(4500, 100);
      waitcnt( CNT + 5000000 );
      BeepHz(4500, 100);

      if( S4_Get(0) == 0xFF )  //Safety check - Allow the user to break out by sending anything else                  
      {
        for( int i=0; i<4; i++ ) {
          Servo32_Set(MotorPin[i], Prefs.MaxThrottle);
        }

        S4_Get(0);

        for( int i=0; i<4; i++ ) {
          Servo32_Set(MotorPin[i], Prefs.MinThrottle);  // Must add 64 to min throttle value (in this calibration code only) if using ESCs with BLHeli version 14.0 or 14.1
        }
      }
    }        
    else if( NudgeMotor == 7 )                         //Motor off (after motor test)
    {
      for( int i=0; i<4; i++ ) {
        Servo32_Set(MotorPin[i], Prefs.MinThrottle);
      }
    }
    NudgeMotor = -1;
    loopTimer = CNT;
  }
  //End Motor test code-----------------------------------
}

void InitializePrefs(void)
{
  Prefs_Load();
  ApplyPrefs();
}  

void ApplyPrefs(void)
{
  Sensors_SetDriftValues( &Prefs.DriftScale[0] );
  Sensors_SetAccelOffsetValues( &Prefs.AccelOffset[0] );
  Sensors_SetMagnetometerScaleOffsets( &Prefs.MagScaleOfs[0] );

  QuatIMU_SetRollCorrection( &Prefs.RollCorrect[0] );
  QuatIMU_SetPitchCorrection( &Prefs.PitchCorrect[0] );

  QuatIMU_SetAutoLevelRates( Prefs.AutoLevelRollPitch , Prefs.AutoLevelYawRate );
  QuatIMU_SetManualRates( Prefs.ManualRollPitchRate , Prefs.ManualYawRate );

#ifdef FORCE_SBUS
  Prefs.UseSBUS = 1;
#endif

#if defined( __V2_PINS_H__ )  // V2 hardware doesn't support the battery monitor
  Prefs.UseBattMon = 0;
#endif
}


void All_LED( int Color )
{
  for( int i=0; i<LED_COUNT; i++ )
    LEDValue[i] = Color;
}  
