/*
 * Compile: make
 * Tools: Sourcery G++ lite with paths set
 * Debug: J-link, Jlink GDB server must be running, loading may fail on first trys
 * Notes:
 * Must stop previous debugging before loading new
 * To view SFR's compile with -g3 and enter SFR register name in expression view, like USART1
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "utils.h"
#include "stm32f2xx.h"
#include "stm32f2xx_gpio.h"
#include "stm32f2xx_tim.h"
#include "stm32f2xx_rcc.h"

#include "sm485.h"
#include "AnalogIn.h"
#include "Serial.h"
#include "mccommunication.h"
#include "FreeRTOS.h"
#include "task.h"
#include "globals.h"
#include "EncoderIn.h"
#include "DSCPowerTask.h"
#include "System.h"
#include "LedBlinkTask.h"
#include "core_cm3.h"
#include "ProductionTester.h"
/*
 * The struct is located just behind vector table at flash.
 *
 * The Struct start is 0x184 from beginning of program flash (+start offset, i.e. 0x08000184 for zero offset or 0x0800c184 for 48kb offset)
 */
extern volatile const FWHeader VSDR_FW_Header __attribute__((section(".firmware_consts")))
=
		{ 0x12345678/*overwritten by makefirmware*/, 9999/*overwritten by makefirmware*/,
				VSDR_HW_ID, VSDR_HW_ID, VSDR_HW_ID, FW_BACKWARDS_COMPATITBLE_VERSION, FW_VERSION,
				0xffffffff };

s32 stepcnt = 0, stepcntsoft = 0;
int prevstat = 0, stat, cyclen = 0;
void SimpleMotionTask( void *pvParameters )
{
	for( ;; )
	{
		if(sys.SMComm.poll()==0) //poll byte from RS485 bus and handle it
			vTaskDelay(1);//no data in buffer and not waiting bytes to arrive, so sleep

		//loopback test, echo rs485 bytes back instantly. comment above poll line if you use this
		//SimpleMotionCommRS485.loopBackComm();
	}
}

/* this task reads some parameters from GC after its been powered up */
void SystemInitTask( void *pvParameters )
{
	for( ;; )
	{
		//take semaphore to block task. task is run once some code gives this semaphore.
		//wait here until GC connection established
		//once initialized, this is executed second time and blocks this task forever
		xSemaphoreTake( SystemInitLauchSemaphore, portMAX_DELAY );

		bool fail = false;

		fail=sys.readInitStateFromGC();
	}
}

void sendPhysInputsToGC()
{
	//send physical input bits to GC side
	u32 send = 0;
	if (sys.physIO.dinGPI1_HomeSwitch.inputState() == true)
		send |= SMP_CB2_HOMESW_ON;
	if (sys.physIO.dinGPI2_EnablePosFeed.inputState() == true)
		send |= SMP_CB2_ENA_POS_FEED;
	if (sys.physIO.dinGPI3_EnableNegFeed.inputState() == true)
		send |= SMP_CB2_ENA_NEG_FEED;
	if (sys.physIO.dinGPI4_ClearFaults.inputState() == true)
		send |= SMP_CB2_CLEARFAULTS;
	if (sys.physIO.dinGPI5_Enable.inputState() == true)
		send |= SMP_CB2_ENABLE;
	sys.setParameter( SMP_CONTROL_BITS2, send );//send command to GC
}

void updatePhysOutputs()
{
#if 0
	if(sys.getDebugParam(4))
	{
#warning TESTIKOODIA
//#error TESTIKOODIA
		sys.physIO.doutGPO1.setState(int(sys.getDebugParam(4)&1));
		sys.physIO.doutGPO2.setState(int(sys.getDebugParam(4)&2));
		sys.physIO.doutGPO3.setState(int(sys.getDebugParam(4)&4));
		sys.physIO.doutGPO4.setState(int(sys.getDebugParam(4)&8));
	}
	else
#endif
	{

	sys.physIO.doutGPO1.setState((sys.GCStatusBits&STAT_RUN) && (sys.GCStatusBits&STAT_INITIALIZED) && !(sys.GCStatusBits&STAT_HOMING) && !(sys.GCStatusBits&STAT_FERROR_RECOVERY));//servo ready
	sys.physIO.doutGPO2.setState(sys.GCStatusBits&STAT_FERROR_WARNING);
	sys.physIO.doutGPO3.setState(sys.GCStatusBits&STAT_FAULTSTOP);
	sys.physIO.doutGPO4.setState(sys.GCStatusBits&STAT_BRAKING);
	}
}

/* this task communicates to GC periodically to do various things
 this task runs at 2500Hz when GC is communicating
 */
void SystemPeriodicTask( void *pvParameters )
{
	int GCUpdateDivider = 0;
	int GCStatusUpdateRate = 2500 / 15; //0.07 sec interval
	bool internalCommErrorForwarded = false;
	u32 prevDigitalInputs = sys.physIO.getGPInputs();


	//take semaphore to block task. task is run once mc comm task code gives this semaphore
	//so this task runs at 2500Hz when GC is communicating
	xSemaphoreTake( SystemPeriodicTaskSemaphore, portMAX_DELAY );
	sendPhysInputsToGC(); //send initial GPI state

	for( ;; )
	{
		bool fail = false;

		//sys.setDebugParam(4,sys.getControlMode());

		//take semaphore to block task. task is run once mc comm task code gives this semaphore
		//so this task runs at 2500Hz when GC is communicating
		xSemaphoreTake( SystemPeriodicTaskSemaphore, portMAX_DELAY );

		//send index pulse position to GC if index encountered from encoder
		if (sys.encoder.hasIndexUpdated())
		{
			if (sys.setParameter( SMP_INDEX_PULSE_LOCATION,
					sys.encoder.getCounterAtIndex() ) == false)
			{
				sys.setFault( FLT_GC_COMM, 100201 );//if setting failed
			}
		}

		//when any GPI digital input changes state, send input state bits to GC
		if (sys.physIO.getGPInputs() != prevDigitalInputs)
		{
			sendPhysInputsToGC();
			prevDigitalInputs = sys.physIO.getGPInputs();
		}

		/*		if(kosh++>20360)
		 {
		 GCUpdateDivider=0;
		 }*/
		/*if(kosh++>45360)
		 {
		 sys.setFault(FLT_GC_COMM,4242);
		 }*/

		/*		{
		 static int dly=0, step=0;
		 if(sys.testPulseAmpl)
		 {
		 if(step==0)
		 sys.setParameter(SMP_ABSOLUTE_POS_TARGET,sys.testPulseAmpl);
		 if(step==1)
		 sys.setParameter(SMP_ABSOLUTE_POS_TARGET,-sys.testPulseAmpl);
		 if(step==2)
		 sys.setParameter(SMP_ABSOLUTE_POS_TARGET,0);
		 }
		 step++;
		 if(step>=2+sys.testPulsePause)
		 step=0;
		 }*/

		//at low rate fetch GC status & fault registers for led blink task and mech brake ctrl
		GCUpdateDivider++;
		if (GCUpdateDivider > GCStatusUpdateRate)
		{
			GCUpdateDivider = 0;

			sys.GCFaultBits = sys.getParameter( SMP_FAULTS, fail );
			sys.GCFirstFault = sys.getParameter( SMP_FIRST_FAULT, fail );
			sys.GCStatusBits = sys.getParameter( SMP_STATUS, fail );

			if (fail)
				sys.setFault( FLT_GC_COMM, 100202 );

			//if IO side gets internal comm error, report it to GC side also for fault stopping
			if ((sys.getFaultBitsReg() & FLT_GC_COMM) && internalCommErrorForwarded == false)
			{
				sys.setParameter( SMP_FAULTS, FLT_GC_COMM );
				internalCommErrorForwarded = true;
			}
		}
	}
}

//lowest priority task for product testing stuff etc that may take long time to execute
//in the wrost case this task may not get execution time as SM task could eat all remaining power at higher priority
void SlowTask( void *pvParameters )
{
	ProductionTester t;
	for(;;)
	{
		vTaskDelay( microsecsToTicks(20000) );

		if(sys.isSignal(System::RunProductionTest))//note: RunProductionTest will remain true until SM command stops it to have STO relays in wanted state
		{
			if(sys.isSignal(System::ProductionTestOver)==false)//to run test only once.
			{
				t.doTests();
			}
			sys.setSignal(System::ProductionTestOver);

//			sys.clrSignal(System::RunProductionTest);//clear after tests as this is read also by other code while test is running
		}

		if (sys.isSignal(System::DeviceReset) )
		{
			vTaskDelay( microsecsToTicks(20000) );//give system some time to send reply packet to SM bus
			GCPSUSetState( false ); //power off GC
			//tried setting reset reason, can't write these apparently
			//RCC->CSR = RCC_CSR_PORRSTF|RCC_CSR_SFTRSTF|RCC_CSR_WDGRSTF|RCC_CSR_WWDGRSTF;
			NVIC_SystemReset(); //never goes past this line
		}
	}
}

/* this task updates device IO's and mech brake status
 */
void UpdateGPIOandBrakeTask( void *pvParameters )
{
	for( ;; )
	{
		vTaskDelay( microsecsToTicks(10000) );

		if(sys.isSignal(System::RunProductionTest)==false)//during tests i/o is switched by testing task so don't touch anything if prod test active
		{
			//control mech brake output
			sys.updateMechBrakeState();

			//refresh optoisolated i/o
			//gsys.updatePhysGPIOs();

			//control general purpose outputs
			updatePhysOutputs();
		}
	}
}

void SimpleMotionBufferedTask( void *pvParameters )
{
	for( ;; )
	{
		/*
		 * wait for MC communication task to call SMComm method to update bus
		 * clock and release this semaphore
		 * (timer tick which ultimately lets this code to run once)
		 */
		if (xSemaphoreTake( sys.SMComm.SimpleMotionBufferedTaskSemaphore, portMAX_DELAY )
				== pdTRUE)
		{
			xSemaphoreTake( sys.SMComm.bufferMutex, portMAX_DELAY );
			sys.SMComm.bufferedCmdUpdate();
			xSemaphoreGive( sys.SMComm.bufferMutex );
		}
	}
}

void vApplicationStackOverflowHook( xTaskHandle *pxTask,
		signed char *pcTaskName )
{
	/* This function will get called if a task overflows its stack.   If the
	 parameters are corrupt then inspect pxCurrentTCB to find which was the
	 offending task. */

	(void) pxTask;
	(void) pcTaskName;

	sys.setFault( FLT_PROGRAM_OR_MEM, 100301 );

	for( ;; )
	{
	}
}

int main( void )
{
	/*
	 At this stage the microcontroller clock setting is already configured,
	 this is done through SystemInit() function which is called from startup
	 file (startup_stm32f2xx.s) before to branch to application main.
	 To reconfigure the default setting of SystemInit() function, refer to
	 system_stm32f2xx.c file
	 */

	/* NVIC configuration */
	/* Configure the Priority Group to 4 bits: 4 bit pre-emption and 0 for subpriority */
	NVIC_PriorityGroupConfig( NVIC_PriorityGroup_4 );

	initGlobals();
	GCPSUInit();


	xTaskCreate( GCPSUTask, (const signed char*)"GCPSUTask", 64, NULL, 4,
			&GCPowerSupplyTaskHandle );
	xTaskCreate( GCCommunicationTask, (const signed char*)"GCCommTask", 256, NULL, 6,
			&GCCommTaskHandle );
	xTaskCreate( LedBlinkTask, (const signed char*)"LedBlinkTask", 64, NULL, 3,
			&ledBlinkTaskHandle );
	xTaskCreate( SystemInitTask, (const signed char*)"SysInitTask", 128, NULL, 2,
			&SystemInitTaskHandle );
	xTaskCreate( SystemPeriodicTask, (const signed char*)"SysPeriodicTk", 128, NULL, 4,
			&SystemPeriodicTaskHandle );
	xTaskCreate( UpdateGPIOandBrakeTask, (const signed char*)"UpdateIOnBrake", 64, NULL, 5,
			&UpdateGPIOandBrakeTaskHandle );
	xTaskCreate( SimpleMotionTask, (const signed char*)"SimpleMotion", 512, NULL, 2,
			&SimpleMotionTaskHandle );
	xTaskCreate( SimpleMotionBufferedTask, (const signed char*)"SimpleMotionBuf", 512, NULL, 3,
			&SimpleMotionBufferedTaskHandle );
	xTaskCreate( SlowTask, (const signed char*)"SlowTask", 128, NULL, 1,
			&SlowTaskHandle );//in the wrost case this task may not get execution time as SM task could eat all remaining power at higher priority


//	sys.setSignal(System::RunProductionTest);


	/* Start the scheduler. */
	vTaskStartScheduler();
}

#ifdef  USE_FULL_ASSERT

/**
 * @brief  This is called from STM library if parameters are incorrect. Use debugger to see call stack to locate error. USE_FULL_ASSERT is configured from makefile.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed( uint8_t* file, uint32_t line )
{
	/* User can add his own implementation to report the file name and line number,
	 ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

	/* Infinite loop */
	while (1)
	{
		sys.setFault(FLT_FIRMWARE,100301);
	}
}
#endif