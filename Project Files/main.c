/**
ECE 4760 Fall 2021
Final Project

AroundSound
Amulya Khurana (ak2425), Aparajito Saha (as2537), Krithik Ranjan (kr397)

Main code for system implementation.
Includes ISRs for distance capture using the HC-SR04 ultrasonic sensor and sound generation using DDS.
Also includes protothreads for receiving button input, sound, distance, and servo motor rotation.
 */

////////////////////////////////////
// clock and protoThreads configure
#include "config_1_3_2.h"
// threading library
#include "pt_cornell_1_3_2.h"

////////////////////////////////////
// graphics libraries
#include "tft_master.h"
#include "tft_gfx.h"
#include <stdlib.h>
#include <math.h>
////////////////////////////////////

////////////////////////////////////
// some precise, fixed, short delays
#define NOP asm("nop");
// 20 cycles 
#define wait20 NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;
// 40 cycles
#define wait40 wait20;wait20;
// 400 cycles = 10 us
#define wait400 wait40;wait40;wait40;wait40;wait40;wait40;wait40;wait40;wait40;wait40;
////////////////////////////////////

// internal pull down for button
#define EnablePullDownB(bits) CNPUBCLR=bits; CNPDBSET=bits;

// system 1 second interval tick
int sys_time_seconds ;
// Variable for tracking time in s since button press
int curr_time_seconds = 0;

// === thread structures ============================================
// HC-SR04 Variables

int trig_flag = 0;

//  the measured period of the wave
short capture1, last_capture1=0, capture_period=99 ;

int capture_flag = 0;

// global variable for distance
float curr_distance = 400;

volatile int button_pressed = 0;
int button_servo_flag = 0;
static int button_state = 0;
static int button_possible = 0;

// the read-pattern if no button is pulled down by an output
#define no_button (0x70)


//======================================================================
// Servo variables
#define SERVO_NEG60 2500
#define SERVO_POS60 5500

int curr_pwm = 2500;

unsigned int pwm_period = 50000;

int servo_thread_flag = 0;

//======================================================================
// 3D audio variables 

#define DAC_config_chan_A 0b0011000000000000
#define DAC_config_chan_B 0b1011000000000000
#define Fs 20000.0
#define two32 4294967296.0 // 2^32 

// starting frequency 
#define C4_f 261.63

#define sustain_constant (_Accum)(256.0/20000.0) ; // seconds per decay update

float Fout;

//== Timer 2 interrupt handler ===========================================
// actual scaled DAC 
volatile unsigned int DAC_data_A, DAC_data_B;
// SPI
volatile SpiChannel spiChn = SPI_CHANNEL2 ;	// the SPI channel to use
volatile int spiClkDiv = 2 ; // 20 MHz max speed for this DAC

// the DDS units: 1=FM, 2=main frequency
volatile unsigned int phase_accum_fm_lt, phase_incr_fm_lt;// 
volatile unsigned int phase_accum_main_lt, phase_incr_main_lt;//'

volatile unsigned int phase_accum_fm_rt, phase_incr_fm_rt;// 
volatile unsigned int phase_accum_main_rt, phase_incr_main_rt;//
// DDS sine table
#define sine_table_size 256
volatile _Accum sine_table[sine_table_size];

// Parameters for FM synthesis
_Accum attack_main  =(_Accum) 0.001;
_Accum attack_fm    =(_Accum) 0.001;
_Accum dk_main      =(_Accum) 0.90;
_Accum dk_fm        =(_Accum) 0.80;
_Accum fm_depth     =(_Accum) 2.2;
volatile _Accum env_fm_lt, env_main_lt, env_fm_rt, env_main_rt;
volatile _Accum dk_state_main_lt, attack_state_main_lt, dk_state_fm_lt, attack_state_fm_lt;
volatile _Accum dk_state_main_rt, attack_state_main_rt, dk_state_fm_rt, attack_state_fm_rt;
volatile _Accum wave_main;

// time scaling for decay calculation
volatile int dk_interval; // wait some samples between decay calcs

volatile _Accum sustain_state, sustain_interval = 0;

// profiling of ISR
volatile int isr_time;

// Variables for sound localization
volatile int ITD_cycles;
volatile int sound_counter = 0;
volatile _Accum IID_atten;

// Variables for distance mapping 
float curr_angle = -60.0;

#define HEAD_RADIUS 0.08

int sound_thread_flag = 0;


int dist_buff[7];
int freq_buff[7];
int ind = 0;

// == Capture 1 ISR ====================================================
// check every cpature for consistency
void __ISR(_INPUT_CAPTURE_1_VECTOR, ipl3) C1Handler(void)
{
    if (trig_flag == 1)
    {
      last_capture1 = mIC1ReadCapture();
      trig_flag = 0;
    }
    else
    {
      capture1 = mIC1ReadCapture();
      capture_period = capture1 - last_capture1;

      capture_flag = 1;
    }

    // clear the timer interrupt flag
    mIC1ClearIntFlag();
}

//==== ISR for DDS =====================================================
void __ISR(_TIMER_4_VECTOR, ipl2) Timer4Handler(void)
{
    mT4ClearIntFlag();
     
     // init the exponential decays
     // by adding energy to the exponential filters 
    
    // envelope calculations are 256 times slower than sample rate
    // computes 4 exponential decays and builds the product envelopes
    if ((dk_interval++ & 0xff) == 0){
        dk_state_fm_lt       = dk_state_fm_lt * dk_fm;
        dk_state_main_lt     = dk_state_main_lt * dk_main;
        attack_state_fm_lt   = attack_state_fm_lt * attack_fm;
        attack_state_main_lt = attack_state_main_lt * attack_main;

        dk_state_fm_rt       = dk_state_fm_rt * dk_fm;
        dk_state_main_rt     = dk_state_main_rt * dk_main;
        attack_state_fm_rt   = attack_state_fm_rt * attack_fm;
        attack_state_main_rt = attack_state_main_rt * attack_main;

        env_fm_lt   = (fm_depth - attack_state_fm_lt) * dk_state_fm_lt;
        env_main_lt = ((_Accum) 1 - attack_state_main_lt) * dk_state_main_lt;

        env_fm_rt   = (fm_depth - attack_state_fm_rt) * dk_state_fm_rt;
        env_main_rt = ((_Accum) 1 - attack_state_main_rt) * dk_state_main_rt;

        if(sustain_state < sustain_interval) {
            dk_state_main_lt = (_Accum) 1;
            dk_state_main_rt = (_Accum) 1;
            sustain_state = sustain_state + sustain_constant ;
        }
    }

    if(curr_angle >= 0)
    {
        // FM phase
        phase_accum_fm_lt += phase_incr_fm_lt; 
        // main phase
        phase_accum_main_lt += phase_incr_main_lt + (sine_table[phase_accum_fm_lt>>24] * env_fm_lt);

        wave_main = (sine_table[phase_accum_main_lt>>24] * env_main_lt);
        // truncate to 12 bits, read table, convert to int and add offset
        DAC_data_A = DAC_config_chan_A | ((int) wave_main + 2048) ; 

        if (sound_counter > ITD_cycles) 
        {
          // FM phase
          phase_accum_fm_rt += phase_incr_fm_rt ; 
          // main phase
          phase_accum_main_rt += phase_incr_main_rt + (sine_table[phase_accum_fm_rt>>24] * env_fm_rt);

          wave_main = (sine_table[phase_accum_main_rt>>24] * env_main_rt);
          if(IID_atten > 0) wave_main = wave_main / IID_atten;
          DAC_data_B = DAC_config_chan_B | ((int) wave_main + 2048) ; 

        }
    }
    else 
    {
        // FM phase
        phase_accum_fm_rt += phase_incr_fm_rt; 
        // main phase
        phase_accum_main_rt += phase_incr_main_rt + (sine_table[phase_accum_fm_rt>>24] * env_fm_rt) ;

        wave_main = (sine_table[phase_accum_main_rt>>24] * env_main_rt);
        // truncate to 12 bits, read table, convert to int and add offset
        DAC_data_B = DAC_config_chan_B| ((int) wave_main + 2048) ; 

        if (sound_counter > ITD_cycles) 
        {
          // FM phase
          phase_accum_fm_lt += phase_incr_fm_lt ; 
          // main phase
          phase_accum_main_lt += phase_incr_main_lt + (sine_table[phase_accum_fm_lt>>24] * env_fm_lt) ;

          wave_main = (sine_table[phase_accum_main_lt>>24] * env_main_lt);
          if(IID_atten > 0) wave_main = (wave_main / IID_atten);
          DAC_data_A = DAC_config_chan_A | ((int) wave_main + 2048); 
        }
    }

    sound_counter++;

    // === Channel A =============
    // CS low to start transaction
     mPORTBClearBits(BIT_4); // start transaction
     // write to spi2
    WriteSPI2(DAC_data_A); 
    while (SPI2STATbits.SPIBUSY); // wait for end of transaction
    mPORTBSetBits(BIT_4); // end transaction

    while (SPI2STATbits.SPIBUSY); // wait for end of transaction
     
    // === Channel B =============
    // CS low to start transaction
     mPORTBClearBits(BIT_4); // start transaction
     // write to spi2
    WriteSPI2(DAC_data_B); 
    while (SPI2STATbits.SPIBUSY); // wait for end of transaction
    mPORTBSetBits(BIT_4); // end transaction
     
    isr_time = max(ReadTimer4(), isr_time); // - isr_time;
} // end ISR TIMER2

// === Button Thread ======================================================
// 
static PT_THREAD (protothread_button(struct pt *pt))
{
    PT_BEGIN(pt);
      static char buffer[128];
      
      while(1) {
          // yield time 30 msec
          PT_YIELD_TIME_msec(30) ;

          // Button push FSM

          // Find the current push button input from IO
          unsigned int push_out = mPORTBReadBits( BIT_7 );
          // BIT 7 set in push_out based on whether push button pressed; right shift by 7 to get
          // 0/1 flag
          push_out = push_out >> 7;
          push_out = !push_out;

          // State NOT_PRESSED: Move to MAYBE_PRESSED if detected push
          if (button_state == 0) 
          {
            if (push_out == 0) {
              button_state = 0;
            }
            else 
            {
              button_state = 1; 
              button_possible = 1;
            }
          }
          // State MAYBE_PRESSED: If push detected again, move to PRESSED, else back to NOT_PRESSED
          // When moving to PRESSED, trigger action using button_pressed flag
          else if (button_state == 1)
          {
            if (push_out == button_possible) {
              button_state = 2;
              button_pressed = 1;
              button_servo_flag = 1;
              ind = 0;
            }
            else 
            {
              button_state = 0;
            }
          }
          // State PRESSED: Key pressed confirmed; stay in this state as long as same key detected
          else if (button_state == 2) 
          {
            button_pressed = 0;
            if (push_out == button_possible) button_state = 2;
            else button_state = 3;
          }
          // State MAYBE_NOT_PRESSED: Push no longer detected, move back to PRESSED if 
          // found again, else move to NOT_PRESSED 
          else
          {
            if (push_out == button_possible) button_state = 2;
            else {
              button_state = 0;
            }
          }
            // NEVER exit while
      } // END WHILE(1)

  PT_END(pt);
} // 

// === Distance Thread ======================================================
// Calculates the current distance by outputting a trigger and measuring 
// duration of echo (done using input capture ISR)
static PT_THREAD (protothread_distance(struct pt *pt))
{
    PT_BEGIN(pt);
    // string buffer
    static char buffer[128];
    
    while(1) {
        // yield time 1 second
        PT_YIELD_TIME_msec(1000);

        // Trigger
        trig_flag = 1;
        mPORTASetBits(BIT_2);
        wait400;
        mPORTAClearBits(BIT_2);

        // Wait until input captured 
        while (!capture_flag);

        // Calculate distance 
        float echo_time = (float)capture_period * 6.4;
        curr_distance = 0.034 * echo_time * 0.5;
        if (curr_distance < 5.0) curr_distance = 5.0;

        dist_buff[ind] = (int)curr_distance;

        capture_flag = 0;

        // Enable sound thread if button has been pressed
        if (button_servo_flag == 1)
          sound_thread_flag = 1;

      } // END WHILE(1)
  PT_END(pt);
} 

// === Sound thread =========================================================
// Thread to produce a FM sound with frequency determined by distance 
static PT_THREAD (protothread_sound(struct pt *pt))
{
    PT_BEGIN(pt);
        static char buffer[128];


      while(1) {
        // yield until button pressed
        PT_YIELD_UNTIL(pt, sound_thread_flag == 1);
        
        sys_time_seconds++ ;
        
        int n;
        if (curr_distance > 200) n = 48; // max distance limit = 2 metres
        else n = 37 - floor(curr_distance / 5.5); 
        
        // Calculate note frequency
        Fout = C4_f * pow(2, n / 12.0);
        freq_buff[ind] = (int)Fout;

        // increment phases
        phase_incr_fm_rt= (int) (3 * Fout * two32/Fs);
        phase_incr_main_rt= (int) (Fout * two32/Fs);

        phase_incr_fm_lt=(int) (3 * Fout * two32/Fs);
        phase_incr_main_lt= (int) (Fout * two32/Fs);

        // Calculate time delay
        float angle = curr_angle * 3.14 / 180;
        float ITD_seconds = (3 * HEAD_RADIUS * sin(angle)) / 340;
        ITD_cycles = abs((int)(ITD_seconds * Fs));

        ind++;

        // Calculate amplitude
        IID_atten = (_Accum)(1 + (pow( Fout / 1000, 0.8) * fabs(sin(angle))));

        // reset FM synthesis variables
        dk_state_fm_lt = fm_depth; 
        dk_state_fm_rt = fm_depth; 
        dk_state_main_lt = (_Accum) 1; 
        dk_state_main_rt = (_Accum) 1; 
        attack_state_fm_lt = fm_depth; 
        attack_state_main_rt = (_Accum) 1; 
        attack_state_fm_lt= fm_depth; 
        attack_state_main_rt = (_Accum) 1; 
        phase_accum_fm_lt = 0;
        phase_accum_main_lt = 0;
        phase_accum_fm_rt = 0;
        phase_accum_main_rt = 0;
        dk_interval = 0;
        sustain_state = 0;

        // Produce the sound
        sound_counter = 0;

        curr_angle += 20;
        if (curr_angle > 60) curr_angle = -60.0;  

        // Enable servo thread
        servo_thread_flag = 1;

        sound_thread_flag = 0;

        // NEVER exit while
      } // END WHILE(1)
  PT_END(pt);
} // thread 4

// === Servo Thread =================================================
// Do a servo sweep from -60 to +60 deg 
static PT_THREAD (protothread_servo(struct pt *pt))
{
    PT_BEGIN(pt);
    while(1) {
      
        // yield until sound produced
        PT_YIELD_UNTIL(pt, servo_thread_flag == 1);

        // Sweep the angle by changing the PWM duty cycle
        
        curr_pwm += 500;
        if (curr_pwm > 5500) 
        {
            curr_pwm = 2500;
            button_servo_flag = 0;
        }
        
        SetDCOC3PWM(curr_pwm);

        servo_thread_flag = 0;

        // NEVER exit while
    } // END WHILE(1)
  PT_END(pt);
} // servo thread

// === Main  ======================================================

int main(void)
{
    //========================================================================
    // SETUP FOR SERVO 

    // === Config timer and output compares to make pulses ========
    // set up timer2 for output capture
    OpenTimer2(T2_ON | T2_SOURCE_INT | T2_PS_1_16, pwm_period);

    // set up compare3 for double compare mode
    OpenOC3(OC_ON | OC_TIMER2_SRC | OC_PWM_FAULT_PIN_DISABLE, 0, 0); //
    // OC3 is PPS group 4, map to RPB9 (pin 18)
    PPSOutput(4, RPB9, OC3);

    // Initialize servo for -60 deg
    SetDCOC3PWM(SERVO_NEG60);

    //========================================================================
    // SETUP FOR DISTANCE SENSOR USING INPUT CAPTURE 

    // Setup TRIG pin to be output and low
    mPORTASetPinsDigitalOut(BIT_2);
    mPORTAClearBits(BIT_2);

    // === Config timer3 free running ==========================
    // set up timer3 as a souce for input capture
    // and let it overflow for contunuous readings
    OpenTimer3(T3_ON | T3_SOURCE_INT | T3_PS_1_256, 0xffff);

    // === set up input capture ================================
    OpenCapture1(  IC_EVERY_EDGE | IC_INT_1CAPTURE | IC_TIMER3_SRC | IC_ON );
    // turn on the interrupt so that every capture can be recorded
    ConfigIntCapture1(IC_INT_ON | IC_INT_PRIOR_3 | IC_INT_SUB_PRIOR_3 );
    INTClearFlag(INT_IC1);
    // connect PIN 24 to IC1 capture unit
    PPSInput(3, IC1, RPB13);

    //========================================================================
    // SETUP FOR 3D SOUND USING DDS, FM, HEAD-RELATED TRANSFER FUNCTION
  
    // Set up timer2 on,  interrupts, internal clock, prescalar 1, toggle rate
    // at 30 MHz PB clock 60 counts is two microsec
    // 400 is 100 ksamples/sec
    // 2000 is 20 ksamp/sec
    OpenTimer4(T4_ON | T4_SOURCE_INT | T4_PS_1_1, 2000);
    // set up the timer interrupt with a priority of 2
    ConfigIntTimer4(T4_INT_ON | T4_INT_PRIOR_2);
    mT4ClearIntFlag(); // and clear the interrupt flag

    // SCK2 is pin 26 
    // SDO2 (MOSI) is in PPS output group 2, could be connected to RB5 which is pin 14
    PPSOutput(2, RPB5, SDO2);

    // control CS for DAC
    mPORTBSetPinsDigitalOut(BIT_4);
    mPORTBSetBits(BIT_4);

    // Configure pins for button
    mPORTBSetPinsDigitalIn(BIT_7);
    EnablePullDownB( BIT_7 );

    
    // divide Fpb by 2, configure the I/O ports. Not using SS in this example
    // 16 bit transfer CKP=1 CKE=1
    // possibles SPI_OPEN_CKP_HIGH;   SPI_OPEN_SMP_END;  SPI_OPEN_CKE_REV
    // For any given peripherial, you will need to match these
    SpiChnOpen(spiChn, SPI_OPEN_ON | SPI_OPEN_MODE16 | SPI_OPEN_MSTEN | SPI_OPEN_CKE_REV , spiClkDiv);

    // turn off the sustain until triggered
    sustain_state = (_Accum)(100.0);
    
    // build the sine lookup table
    // scaled to produce values between 0 and 4096
    int i;
    for (i = 0; i < sine_table_size; i++){
            sine_table[i] =  (_Accum)(2047*sin((float)i*6.283/(float)sine_table_size));
    }
  
    // === config the uart, DMA, vref, timer5 ISR ===========
    PT_setup();

    // === setup system wide interrupts  ====================
    INTEnableSystemMultiVectoredInt();
    
    pt_add(protothread_button, 1);
    pt_add(protothread_distance, 1);
    // pt_add(protothread_time, 1);
    pt_add(protothread_sound, 1);
    pt_add(protothread_servo, 1);


    // === initalize the scheduler ====================
    PT_INIT(&pt_sched) ;
    // >>> CHOOSE the scheduler method: <<<
    // (1)
    // SCHED_ROUND_ROBIN just cycles thru all defined threads
    //pt_sched_method = SCHED_ROUND_ROBIN ;
    
    // (2)
    // SCHED_RATE executes some threads more often then others
    // -- rate=0 fastest, rate=1 half, rate=2 quarter, rate=3 eighth, rate=4 sixteenth,
    // -- rate=5 or greater DISABLE thread!
    // pt_sched_method = SCHED_RATE ;
    
    pt_sched_method = SCHED_ROUND_ROBIN ;
    
    // === scheduler thread =======================
    // scheduler never exits
    PT_SCHEDULE(protothread_sched(&pt_sched));
    // ============================================
} // main