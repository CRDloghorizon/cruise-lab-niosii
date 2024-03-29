//zhu wenyao ht18
//cruise.c mahiru
#include <stdio.h>
#include "system.h"
#include "includes.h"
#include "altera_avalon_pio_regs.h"
#include "sys/alt_irq.h"
#include "sys/alt_alarm.h"
#include "time.h"
#include "sys/alt_timestamp.h"
#include "sys/alt_cache.h"

extern void delay_asm (int millisec); //delay for 1ms

#define DEBUG 1

#define HW_TIMER_PERIOD 100 /* 100ms */

/* Button Patterns */

#define GAS_PEDAL_FLAG      0x08
#define BRAKE_PEDAL_FLAG    0x04
#define CRUISE_CONTROL_FLAG 0x02
/* Switch Patterns */

#define TOP_GEAR_FLAG       0x00000002
#define ENGINE_FLAG         0x00000001
//define GEARENG_FLAG        0x00000003

/* LED Patterns */

#define LED_RED_0 0x00000001 // Engine
#define LED_RED_1 0x00000002 // Top Gear
#define LED_RED_2 0x00000003 // both engine and gear

#define LED_GREEN_0 0x0001 // Cruise Control activated
#define LED_GREEN_2 0x0004 // Cruise Control Button
#define LED_GREEN_4 0x0010 // Brake Pedal
#define LED_GREEN_6 0x0040 // Gas Pedal

/*
 * Definition of Tasks
 */

#define TASK_STACKSIZE 2048

OS_STK StartTask_Stack[TASK_STACKSIZE]; 
OS_STK ControlTask_Stack[TASK_STACKSIZE]; 
OS_STK VehicleTask_Stack[TASK_STACKSIZE];
OS_STK ButtonIO_Stack[TASK_STACKSIZE];
OS_STK SwitchIO_Stack[TASK_STACKSIZE];

OS_STK Watchdog_Stack[TASK_STACKSIZE];
OS_STK Overload_Stack[TASK_STACKSIZE];
OS_STK Extraload_Stack[TASK_STACKSIZE];

// Task Priorities
 
#define WATCHDOG_PRIO      1

#define SWITCHIO_PRIO      3
#define BUTTONIO_PRIO      4

#define STARTTASK_PRIO     5


#define EXTRALOAD_PRIO     7
#define OVERLOAD_PRIO      12 //lowest

#define VEHICLETASK_PRIO   9
#define CONTROLTASK_PRIO   10

// Task Periods

#define CONTROL_PERIOD  300
#define VEHICLE_PERIOD  300

/*
 * Definition of Kernel Objects 
 */

// Mailboxes
OS_EVENT *Mbox_Throttle;
OS_EVENT *Mbox_Velocity;

// Semaphores  --total nana semaphores for each task
OS_EVENT *VehicleSem;
OS_EVENT *ControlSem;
OS_EVENT *ButtonIOSem;
OS_EVENT *SwitchIOSem;
OS_EVENT *WatchdogSem;
OS_EVENT *OverloadSem;
OS_EVENT *ExtraloadSem;

// SW-Timer
OS_TMR *vehicle_timer;
OS_TMR *control_timer;
OS_TMR *buttonio_timer;
OS_TMR *switchio_timer;
OS_TMR *watchdog_timer;
OS_TMR *overload_timer;
OS_TMR *extraload_timer;

/*
 * Types
 */
enum active {on, off};

enum active gas_pedal = off;
enum active brake_pedal = off;
enum active top_gear = off;
enum active engine = off;
enum active cruise_control = off; 
enum active cruise_active = off; 

/*
 * Global variables
 */
int delay; // Delay of HW-timer 
INT16U led_green = 0; // Green LEDs
INT32U led_red = 0;   // Red LEDs


INT8U switch_value=0;
INT8U utilization=1;
INT8U overloadflag=1;
INT16U targetvelocity=0; //target velocity to maintain


//call back function *(7)
void vehiclecallback(void *ptmr, void *callback_arg)
{
    OSSemPost(VehicleSem);
}
void controlcallback(void *ptmr, void *callback_arg)
{
    OSSemPost(ControlSem);
}
void buttoniocallback(void *ptmr, void *callback_arg)
{
    OSSemPost(ButtonIOSem);
}
void switchiocallback(void *ptmr, void *callback_arg)
{
    OSSemPost(SwitchIOSem);
}
void watchdogcallback(void *ptmr, void *callback_arg)
{
    OSSemPost(WatchdogSem);
}
void overloadcallback(void *ptmr, void *callback_arg)
{
    OSSemPost(OverloadSem);
}
void extraloadcallback(void *ptmr, void *callback_arg)
{
    OSSemPost(ExtraloadSem);
}




/* void callbackV (void *ptmr, void *callback_arg)
{
    if(flag==1)
    {
        OSSemPost (sem1);
        //printf("v\n");
    }
    if(flag==2)
    {
        OSSemPost (sem2);
        //printf("c\n");
    }
}
void callbackExtraload(void* ptmr, void* callback_arg)
{
    OSSemPost(Extraloadsem);
}
*/



int buttons_pressed(void)
{
  return ~IORD_ALTERA_AVALON_PIO_DATA(D2_PIO_KEYS4_BASE);
}

int switches_pressed(void)
{
  return IORD_ALTERA_AVALON_PIO_DATA(DE2_PIO_TOGGLES18_BASE);    
}

/*
 * ISR for HW Timer
 */
alt_u32 alarm_handler(void* context)
{
  OSTmrSignal(); /* Signals a 'tick' to the SW timers */
  
  return delay;
}

static int b2sLUT[] = {0x40, //0
               0x79, //1
               0x24, //2
               0x30, //3
               0x19, //4
               0x12, //5
               0x02, //6
               0x78, //7
               0x00, //8
               0x18, //9
               0x3F, //-
};

/*
 * convert int to seven segment display format
 */
int int2seven(int inval){
  return b2sLUT[inval];
}

/*
 * output current velocity on the seven segement display
 */
void show_velocity_on_sevenseg(INT8S velocity){
  int tmp = velocity;
  int out;
  INT8U out_high = 0;
  INT8U out_low = 0;
  INT8U out_sign = 0;

  if(velocity < 0){
    out_sign = int2seven(10);
    tmp *= -1;
  }else{
    out_sign = int2seven(0);
  }

  out_high = int2seven(tmp / 10);
  out_low = int2seven(tmp - (tmp/10) * 10);
  
  out = int2seven(0) << 21 |
    out_sign << 14 |
    out_high << 7  |
    out_low;
  IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_HEX_LOW28_BASE,out);
}

/*
 * shows the target velocity on the seven segment display (HEX5, HEX4)
 * when the cruise control is activated (0 otherwise)
 */
void show_target_velocity(INT8U target_vel)
{
    int tmp = target_vel;
    int out;
    INT8U out_high = 0;
    INT8U out_low = 0;
    
    out_high = int2seven(tmp / 10);
    out_low = int2seven(tmp - (tmp/10) * 10);
    
    out = int2seven(0) << 21 | int2seven(0) << 14 | out_high << 7 | out_low;
    
    IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_HEX_HIGH28_BASE,out);
}

/*
 * indicates the position of the vehicle on the track with the four leftmost red LEDs
 * LEDR17: [0m, 400m)
 * LEDR16: [400m, 800m)
 * LEDR15: [800m, 1200m)
 * LEDR14: [1200m, 1600m)
 * LEDR13: [1600m, 2000m)
 * LEDR12: [2000m, 2400m]
 */
 //may need modify
void show_position(INT16U position)
{
    if (position >=0 && position <4000)
        led_red = (led_red & 0x20000) | 0x20000;
    if (position >=4000 && position < 8000)
        led_red = (led_red & 0x10000) | 0x10000;
    if (position >=8000 && position < 12000)
        led_red = (led_red & 0x08000) | 0x08000;
    if (position >=12000 && position < 16000)
        led_red = (led_red & 0x04000) | 0x04000;
    if (position >=16000 && position < 20000)
        led_red = (led_red & 0x02000) | 0x02000;
    if (position >=20000 && position < 24000)
        led_red = (led_red & 0x01000) | 0x01000;
    
    //only one pio red led output
    //IOWR_ALTERA_AVALON_PIO_DATA (DE2_PIO_REDLED18_BASE, led_red);
}

/*
 * The function 'adjust_position()' adjusts the position depending on the
 * acceleration and velocity.
 */
INT16U adjust_position(INT16U position, INT16S velocity,
               INT8S acceleration, INT16U time_interval)
{
  INT16S new_position = position + velocity * time_interval / 1000
    + acceleration / 2  * (time_interval / 1000) * (time_interval / 1000);

  if (new_position > 24000) {
    new_position -= 24000;
  } else if (new_position < 0){
    new_position += 24000;
  }
  
  show_position(new_position);  //here show position

  return new_position;
}
 
/*
 * The function 'adjust_velocity()' adjusts the velocity depending on the
 * acceleration.
 */
INT16S adjust_velocity(INT16S velocity, INT8S acceleration,  
               enum active brake_pedal, INT16U time_interval)
{
  INT16S new_velocity;
  INT8U brake_retardation = 200;

  if (brake_pedal == off)
    new_velocity = velocity  + (float) (acceleration * time_interval) / 1000.0;
  else {
    if (brake_retardation * time_interval / 1000 > velocity)
      new_velocity = 0;
    else
      new_velocity = velocity - brake_retardation * time_interval / 1000;
  }
  
  return new_velocity;
}

/*
 * The task 'VehicleTask' updates the current velocity of the vehicle
 */
void VehicleTask(void* pdata)
{ 
  INT8U err;  
  void* msg;
  INT8U* throttle; 
  INT8S acceleration;  /* Value between 40 and -20 (4.0 m/s^2 and -2.0 m/s^2) */
  INT8S retardation;   /* Value between 20 and -10 (2.0 m/s^2 and -1.0 m/s^2) */
  INT16U position = 0; /* Value between 0 and 20000 (0.0 m and 2000.0 m)  */
  INT16S velocity = 0; /* Value between -200 and 700 (-20.0 m/s amd 70.0 m/s) */
  INT16S wind_factor;   /* Value between -10 and 20 (2.0 m/s^2 and -1.0 m/s^2) */

  printf("Vehicle task created!\n");

  while(1)
    {
      OSSemPend(VehicleSem,0,&err);
      err = OSMboxPost(Mbox_Velocity, (void *) &velocity);

      //OSTimeDlyHMSM(0,0,0,VEHICLE_PERIOD);

      /* Non-blocking read of mailbox: 
     - message in mailbox: update throttle
     - no message:         use old throttle
      */
      msg = OSMboxPend(Mbox_Throttle, 1, &err);
       //here task want to receive the message
      if (err == OS_NO_ERR) 
          throttle = (INT8U*) msg;

      /* Retardation : Factor of Terrain and Wind Resistance */
      if (velocity > 0)
          wind_factor = velocity * velocity / 10000 + 1;
      else 
          wind_factor = (-1) * velocity * velocity / 10000 + 1;
         
      if (position < 4000) 
          retardation = wind_factor; // even ground
      else if (position < 8000)
          retardation = wind_factor + 15; // traveling uphill
      else if (position < 12000)
          retardation = wind_factor + 25; // traveling steep uphill
      else if (position < 16000)
          retardation = wind_factor; // even ground
      else if (position < 20000)
          retardation = wind_factor - 10; //traveling downhill
      else
          retardation = wind_factor - 5 ; // traveling steep downhill
                  
      acceleration = *throttle / 2 - retardation;     
      position = adjust_position(position, velocity, acceleration, 300); 
      velocity = adjust_velocity(velocity, acceleration, brake_pedal, 300); 
      printf("Position: %dm\n", position / 10);
      printf("Velocity: %4.1fm/s\n", velocity /10.0);
      printf("Throttle: %dV\n", *throttle / 10);
      show_velocity_on_sevenseg((INT8S) (velocity / 10));
      //show_position((INT16U)position); on adjust part

    }
} 
 
/*
 * The task 'ControlTask' is the main task of the application. It reacts
 * on sensors and generates responses.
 */

void ControlTask(void* pdata)
{
  INT8U err;
  INT8U throttle = 40; /* Value between 0 and 80, which is interpreted as between 0.0V and 8.0V */
  void* msg;
  INT16S* current_velocity;
  
  printf("Control Task created!\n");
  
  INT8U cruisebit=0; //show the cruise state
  INT8U controltag=1;
  //INT16S deviation=0; //deviation of v


  while(1)
    {
          
          OSSemPend (ControlSem, 0, &err);
          msg = OSMboxPend(Mbox_Velocity, 0, &err);
          current_velocity = (INT16S*) msg;
          
          cruisebit = (engine==on) && (cruise_control==on) && (top_gear==on) && (*current_velocity >= 200); // 1=cruise, 0=not
          
          if (engine == off)
          {
        	  throttle=0;
              cruise_active=off;
              cruise_control=off;
              brake_pedal = on;  //make sure that the car is stop when engine is off
          }
          else if(engine == on)
          {
              if(cruisebit==1)
              {
                  cruise_active = on;
                  if(controltag==1) //here decide which target velocity to maintain
                  {
                      targetvelocity = *current_velocity;
                      controltag = 0;
                  }
                  
                  if(*current_velocity <= targetvelocity)
                  {
                      if ((throttle+20)<80)
                      {throttle = throttle+20;}
                      else
                      {throttle = 80;}
                  }
                  else
                  {
                      if ((throttle-20)>0)
                      {throttle = throttle-20;}
                      else
                      {throttle = 0;}
                  }
              }
              else  //deatached the cruise control
              {
                  if(gas_pedal==on && top_gear==on) //gas pedal is controlled here
                  {
                      throttle = 80;
                  }
                  else if(gas_pedal==on && top_gear==off)
                  {
                      throttle = 40;
                  }
                  else
                  {
                	  throttle = 20;
                  }
                  controltag = 1;   //deatached the cruise control
                  targetvelocity = 0;
                  cruise_control= off;
                  cruise_active = off;
              } //quit cruise mode, reset throttle
          }

//OSTimeDlyHMSM(0,0,0, CONTROL_PERIOD);
          show_target_velocity((INT8U)(targetvelocity/10));
          printf("%d, %d\n",throttle,(int)(targetvelocity/10));
          err = OSMboxPost(Mbox_Throttle, (void *) &throttle);

    }
}



//ButtonIO
void ButtonIO(void* pdata)
{
    INT8U err;
    int value;
    printf("start button IO\n");
    
    while(1)
    {
        led_green=0;
        OSSemPend(ButtonIOSem,0,&err);
        
        value = buttons_pressed();
        value = value & 0xf;
        // if cruise is activated, then greenled0 on
        if(cruise_active==on)
        {
            led_green = LED_GREEN_0;
            //printf("active\n");
        }
        else
        {
            led_green = 0;
        }

        switch (value)
        {
            case CRUISE_CONTROL_FLAG:  //0x02 key1 ledg2
                led_green = led_green|LED_GREEN_2;
                cruise_control=on;
                brake_pedal = off;
                gas_pedal = off;
                break;
            case BRAKE_PEDAL_FLAG:  //0x04 key2 ledg4
                led_green = led_green|LED_GREEN_4;
                cruise_control=off;
                brake_pedal = on;
                gas_pedal = off;
                break;
            case GAS_PEDAL_FLAG:  //0x08 key3 ledg6
                led_green = led_green|LED_GREEN_6;
                cruise_control=off;
                brake_pedal = off;
                gas_pedal = on;
                break;
            default:
                brake_pedal = off;
                gas_pedal = off;
                break;
        }
        IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_GREENLED9_BASE, led_green); //led green output

    }

}


//SwitchIO
void SwitchIO(void* pdata)
{
    INT8U err;
    int value;
    int sw;
    int temp = 0;
    printf("start switch IO\n");
    while(1)
    {
        OSSemPend(SwitchIOSem,0,&err);
        
        value = switches_pressed();
        sw = value & 0x3;
        switch_value =(INT8U) ((value >> 4)&0x3f);  //overload signal sw4 to 9
        
        if(sw == ENGINE_FLAG)    // engine on
        {
            temp = LED_RED_0;  //first 6 led are used
            engine = on;
            top_gear = off;
        }
        else if(sw == TOP_GEAR_FLAG)
        {
        	temp = LED_RED_1;
            engine = off;
            top_gear = on;
            //printf("top gear but no engine.\n");
        }
        else if(sw == 0x3)
        {
        	temp = LED_RED_2;
        	engine = on;
        	top_gear = on;

        }
        else
        {
            temp = 0;
        	engine = off;
            top_gear = off;

        }
        if(switch_value < 50)
        {
            utilization = switch_value * 2;
        }
        else if(switch_value >= 50)
        {
            utilization = 100;
        }
        else
        {
            utilization = 1;
        }
        
        //printf("get value %d\n",value);
        IOWR_ALTERA_AVALON_PIO_DATA(DE2_PIO_REDLED18_BASE, led_red|temp); //led red output
    }

}

//watch dog
void Watchdog (void *pdata)
{
    INT8U err;
    INT32U rinnetime; //time from last watch dog section
    INT8U warning;
    while(1)
    {
        OSSemPend(WatchdogSem,0,&err);
        if (overloadflag)
        {
            rinnetime = OSTimeGet();
            overloadflag = 0;
            warning = 0;
        }
        else
        {
            if(OSTimeGet()-rinnetime > 1000)  //1000ms delay before going to the next round == overload
            {
                warning = 1;
            }
            else
            {
                warning = 0;
            }
        }
        if(warning)
        {
            printf("warning, overload happened.\n");
        }
    }
}

//extraload
void Extraload(void *pdata)
{
    INT8U err;
    INT32U util;

    while(1)
    {
        OSSemPend (ExtraloadSem, 0, &err);
        util = utilization*3;   //percentage in 300ms
        printf("ExtraLoadTask: Utilization (%d) %%, delay %d ms.\n",(int)utilization,(int)util);
        delay_asm((int) util);
    }

}

//overload detection
void Overload (void *pdata)     // Overload_Detection 
{
    INT8U err;
    while(1)
    {
        OSSemPend(OverloadSem,0,&err);
        overloadflag = 1;
    }
}

/* 
 * The task 'StartTask' creates all other tasks kernel objects and
 * deletes itself afterwards.
 */ 

void StartTask(void* pdata)
{
  INT8U err;
  INT8U perr;
  void* context;

  static alt_alarm alarm;     /* Is needed for timer ISR function */
  
  /* Base resolution for SW timer : HW_TIMER_PERIOD ms */
  delay = alt_ticks_per_second() * HW_TIMER_PERIOD / 1000; 
  printf("delay in ticks %d\n", delay);

  /* 
   * Create Hardware Timer with a period of 'delay' 
   */
  if (alt_alarm_start (&alarm,
               delay,
               alarm_handler,
               context) < 0)
    {
      printf("No system clock available!n");
    }

  /* 
   * Create and start Software Timer 
   */
  vehicle_timer= OSTmrCreate(0,    //Initial delay
                      3, // period
                      OS_TMR_OPT_PERIODIC,
                      vehiclecallback,
                      NULL,
                      "vehicle_timer",
                      &err);
  control_timer= OSTmrCreate(0,    //Initial delay
                      3, // period
                      OS_TMR_OPT_PERIODIC,
                      controlcallback,
                      NULL,
                      "control_timer",
                      &err);
  buttonio_timer= OSTmrCreate(0,    //Initial delay
                      3, // period
                      OS_TMR_OPT_PERIODIC,
                      buttoniocallback,
                      NULL,
                      "buttonio_timer",
                      &err);
  switchio_timer= OSTmrCreate(0,    //Initial delay
                      3, // period
                      OS_TMR_OPT_PERIODIC,
                      switchiocallback,
                      NULL,
                      "switchio_timer",
                      &err);
  watchdog_timer= OSTmrCreate(0,    //Initial delay
                      3, // period
                      OS_TMR_OPT_PERIODIC,
                      watchdogcallback,
                      NULL,
                      "watchdog_timer",
                      &err);
  extraload_timer=OSTmrCreate(0,    //Initial delay
                              3, // period 3
                              OS_TMR_OPT_PERIODIC,
                              extraloadcallback,
                              NULL,
                              "extraload_timer",
                              &err);
  overload_timer= OSTmrCreate(0,    //Initial delay
                      3, // period
                      OS_TMR_OPT_PERIODIC,
                      overloadcallback,
                      NULL,
                      "overload_timer",
                      &err);
  
  OSTmrStart (vehicle_timer,&perr);
  OSTmrStart (control_timer,&perr);
  OSTmrStart (buttonio_timer,&perr);
  OSTmrStart (switchio_timer,&perr);
  OSTmrStart (watchdog_timer,&perr);
  OSTmrStart (extraload_timer,&perr);
  OSTmrStart (overload_timer,&perr);
  
  
  
  
  /*
   * Creation of Kernel Objects
   */
  
  // Mailboxes
  Mbox_Throttle = OSMboxCreate((void*) 0); /* Empty Mailbox - Throttle */
  Mbox_Velocity = OSMboxCreate((void*) 0); /* Empty Mailbox - Velocity */
   
  //semaphores
  VehicleSem = OSSemCreate(1); //vehicle
  ControlSem = OSSemCreate(1); //control
  ButtonIOSem = OSSemCreate(1);
  SwitchIOSem = OSSemCreate(1);
  WatchdogSem = OSSemCreate(1);
  ExtraloadSem = OSSemCreate(1);
  OverloadSem = OSSemCreate(1);

  /*
   * Create statistics task
   */

  OSStatInit();

  /* 
   * Creating Tasks in the system 
   */


  err = OSTaskCreateExt(
            ControlTask, // Pointer to task code
            NULL,        // Pointer to argument that is
                    // passed to task
            &ControlTask_Stack[TASK_STACKSIZE-1], // Pointer to top
            // of task stack
            CONTROLTASK_PRIO,
            CONTROLTASK_PRIO,
            (void *)&ControlTask_Stack[0],
            TASK_STACKSIZE,
            (void *) 0,
            OS_TASK_OPT_STK_CHK);

  err = OSTaskCreateExt(
            VehicleTask, // Pointer to task code
            NULL,        // Pointer to argument that is
                    // passed to task
            &VehicleTask_Stack[TASK_STACKSIZE-1], // Pointer to top
            // of task stack
            VEHICLETASK_PRIO,
            VEHICLETASK_PRIO,
            (void *)&VehicleTask_Stack[0],
            TASK_STACKSIZE,
            (void *) 0,
            OS_TASK_OPT_STK_CHK);
 //button io
  err = OSTaskCreateExt(
              ButtonIO, // Pointer to task code
              NULL,        // Pointer to argument that is
                      // passed to task
              &ButtonIO_Stack[TASK_STACKSIZE-1], // Pointer to top
              // of task stack
              BUTTONIO_PRIO,
              BUTTONIO_PRIO,
              (void *)&ButtonIO_Stack[0],
              TASK_STACKSIZE,
              (void *) 0,
              OS_TASK_OPT_STK_CHK);
  
  //switch io
  err = OSTaskCreateExt(
              SwitchIO, // Pointer to task code
              NULL,        // Pointer to argument that is
                      // passed to task
              &SwitchIO_Stack[TASK_STACKSIZE-1], // Pointer to top
              // of task stack
              SWITCHIO_PRIO,
              SWITCHIO_PRIO,
              (void *)&SwitchIO_Stack[0],
              TASK_STACKSIZE,
              (void *) 0,
              OS_TASK_OPT_STK_CHK);
  
  //watch dog
  err = OSTaskCreateExt(
              Watchdog,    // Pointer to task code
              NULL,        // Pointer to argument that is
                           // passed to task
              &Watchdog_Stack[TASK_STACKSIZE-1], // Pointer to top
                                    // of task stack
              WATCHDOG_PRIO,
              WATCHDOG_PRIO,
              (void *)&Watchdog_Stack[0],
              TASK_STACKSIZE,
              (void *) 0,
              OS_TASK_OPT_STK_CHK);
       
  //overload
  err = OSTaskCreateExt(
              Overload,    // Pointer to task code
              NULL,        // Pointer to argument that is
                           // passed to task
              &Overload_Stack[TASK_STACKSIZE-1], // Pointer to top
                                    // of task stack
              OVERLOAD_PRIO,
              OVERLOAD_PRIO,
              (void *)&Overload_Stack[0],
              TASK_STACKSIZE,
              (void *) 0,
              OS_TASK_OPT_STK_CHK);
       
  //extraload
  err = OSTaskCreateExt(
              Extraload,    // Pointer to task code
              NULL,        // Pointer to argument that is
                           // passed to task
              &Extraload_Stack[TASK_STACKSIZE-1], // Pointer to top
                                    // of task stack
              EXTRALOAD_PRIO,
              EXTRALOAD_PRIO,
              (void *)&Extraload_Stack[0],
              TASK_STACKSIZE,
              (void *) 0,
              OS_TASK_OPT_STK_CHK);

  printf("All Tasks and Kernel Objects generated!\n");

  /* Task deletes itself */

  OSTaskDel(OS_PRIO_SELF);
}

/*
 *
 * The function 'main' creates only a single task 'StartTask' and starts
 * the OS. All other tasks are started from the task 'StartTask'.
 *
 */

int main(void) {

  printf("Lab: Cruise Control\n");
 
  OSTaskCreateExt(
          StartTask, // Pointer to task code
          NULL,      // Pointer to argument that is
          // passed to task
          (void *)&StartTask_Stack[TASK_STACKSIZE-1], // Pointer to top
          // of task stack 
          STARTTASK_PRIO,
          STARTTASK_PRIO,
          (void *)&StartTask_Stack[0],
          TASK_STACKSIZE,
          (void *) 0,  
          OS_TASK_OPT_STK_CHK | OS_TASK_OPT_STK_CLR);
         
  OSStart();
  
  return 0;
}
