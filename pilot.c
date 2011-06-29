/*
 * A simple Arduino Duemilanove-based quadcopter autopilot.
 *
 * Licensed under AGPLv3.
 */

#include <avr/io.h>
#include <avr/interrupt.h>

#include "adc.h"
#include "timer1.h"
#include "uart.h"
#include "actuators.h"
#include "rx.h"
#include "twi.h"
#include "cmps09.h"
#include "ahrs.h"
#include "isqrt.h"

static uint8_t motor[4] = { 0, 0, 0, 0 };

static void show_state(void) {
	serial_write_dec8(motor[0]);
	serial_write_dec8(motor[1]);
	serial_write_dec8(motor[2]);
	serial_write_dec8(motor[3]);
	serial_write_eol();
}

static void handle_input(char ch) {
	switch (ch) {
#define MOTOR_DOWN(n)				\
		if (motor[n] > 0)		\
			motor[n] --;		\
		actuator_set(n, motor[n] << 8);	\
		break;
#define MOTOR_UP(n)				\
		if (motor[n] < 255)		\
			motor[n] ++;		\
		actuator_set(n, motor[n] << 8);	\
		break;
	case 'a':
		MOTOR_DOWN(0);
	case 's':
		MOTOR_DOWN(1);
	case 'd':
		MOTOR_DOWN(2);
	case 'f':
		MOTOR_DOWN(3);
	case 'q':
		MOTOR_UP(0);
	case 'w':
		MOTOR_UP(1);
	case 'e':
		MOTOR_UP(2);
	case 'r':
		MOTOR_UP(3);
	default:
		return;
	}

	sei();
	show_state();
}

void nop(void) {}
void die(void) {
	cli();
	serial_write_str("ERROR");
	while (1);
}

void setup(void) {
	uint8_t s = SREG;
	uint8_t m = MCUCR;
	uint8_t ver, cnt, regs[6];
	int16_t v;
	uint32_t len;

	/* Initialise everything we need */
	serial_init();
	adc_init();
	timer_init();
	actuators_init(4);
	serial_set_handler(handle_input);
	rx_init();
	twi_init();
	sei();

	adc_convert_all(nop);

	rx_no_signal = 10;

	/* Wait for someone to attach to UART */
	my_delay(4000);

	serial_write_str("SREG:");
	serial_write_hex16(s);
	serial_write_str(", MCUCR:");
	serial_write_hex16(m);
	serial_write_eol();

	/* Perform all the status sanity checks */

	serial_write_str("Battery voltage:");
	/* Reference volatage is 3.3V & resistors divide input voltage by ~5 */
	serial_write_fp32((uint32_t) adc_values[3] * 323 * (991 + 241),
			0x400L * 100 * 241);
	serial_write1('V');
	serial_write_eol();
	/* TODO: check that li-poly voltage is not below 3.2V per cell
	 * (unless Li-Po-Fe) */

	serial_write_str("CPU temperature:");
	/* Reference volatage is 1.1V now */
	serial_write_fp32((adc_values[4] - 269) * 1100, 0x400);
	serial_write_eol();

	ver = 0xff;
	cmps09_read_bytes(0, 1, &ver);
	serial_write_str("Magnetometer revision:");
	serial_write_hex16(ver);
	serial_write_eol();
	if (ver != 0x02)
		die();

	serial_write_str("Checking if gyro readings in range.. ");
	/* 1.23V expected -> 2 * 0x400 * 1.23V / 3.3V == 0x2fb */
	cnt = 0;
	while (adc_values[0] > 0x2a0 && adc_values[0] < 0x350 &&
			adc_values[1] > 0x2a0 && adc_values[1] < 0x350 &&
			adc_values[2] > 0x2a0 && adc_values[2] < 0x350 &&
			cnt ++ < 20) {
		adc_values[0] = 2 * adc_convert(0);
		adc_values[1] = 2 * adc_convert(1);
		adc_values[2] = 2 * adc_convert(2);
	}
	if (cnt < 21)
		die();
	serial_write_str("yep");
	serial_write_eol();

	serial_write_str("Checking magnetic field magnitude.. ");
	cmps09_read_bytes(10, 6, regs);
	v = (((uint16_t) regs[0] << 8) | regs[1]) - cmps09_mag_calib[0];
	len = (int32_t) v * v;
	v = (((uint16_t) regs[2] << 8) | regs[3]) - cmps09_mag_calib[1];
	len += (int32_t) v * v;
	v = (((uint16_t) regs[4] << 8) | regs[5]) - cmps09_mag_calib[2];
	len += (int32_t) v * v;
	len = isqrt32(len);
	serial_write_fp32(len, 1000);
	serial_write_str(" T");
	serial_write_eol();
	if (len > 600 || len < 300)
		die();

	serial_write_str("Checking accelerometer readings.. ");
	v = 0;
	for (cnt = 0; cnt < 16; cnt ++) {
		cmps09_read_bytes(16, 6, regs);
		v = ((int16_t) (((uint16_t) regs[0] << 8) | regs[1]) + 1) >> 1;
		len += (int32_t) v * v;
		v = ((int16_t) (((uint16_t) regs[2] << 8) | regs[3]) + 1) >> 1;
		len += (int32_t) v * v;
		v = ((int16_t) (((uint16_t) regs[4] << 8) | regs[5]) + 1) >> 1;
		len += (int32_t) v * v;
		my_delay(20);
	}
	len = (isqrt32(len) + 1) >> 1;
	/* TODO: the scale seems to change a lot with temperature? */
	serial_write_fp32(len, 0x4050);
	serial_write_str(" g");
	serial_write_eol();
	if (len > 0x4070 || len < 0x3f00)
		die();

	serial_write_str("Receiver signal: ");
	serial_write_str(rx_no_signal ? "NOPE" : "yep");
	serial_write_eol();
	if (!rx_no_signal && rx_co_throttle > 5) {
		serial_write_str("Throttle stick is not in the bottom "
				"position\r\n");
		die();
	}

	serial_write_str("Calibrating sensors..\r\n");

	/* Start the software clever bits */
	ahrs_init();
	actuators_start();

	serial_write_str("AHRS loop and actuator signals are running\r\n");

	show_state();
}

/* The modes are a set of boolean switches that can be set or reset/cleared
 * using the "gyro switch" on the transmitter.  Every move of the switch
 * changes the mode pointed at by the "CH5" potentiometer which is on the
 * right side of the transmitter.
 */
enum modes_e {
	MODE_MOTORS_ARMED,	/* TODO: Arm/disarm the motors, too risky? */
	MODE_HEADINGHOLD_ENABLE,/* Enable compass-based heading-hold */
	MODE_PANTILT_ENABLE	/* TODO: Cyclic stick controls the pan&tilt */
};
static uint8_t modes =
	(0 << MODE_MOTORS_ARMED) |
	(0 << MODE_HEADINGHOLD_ENABLE) |
	(0 << MODE_PANTILT_ENABLE);
static uint8_t prev_sw = 0;

void modes_update(void) {
	uint8_t num;

	if (rx_gyro_sw == prev_sw)
		return;
	prev_sw = rx_gyro_sw;

	num = ((uint16_t) rx_right_pot + 36) / 49;
	modes &= 1 << num;
	modes |= prev_sw << num;
}

void control_update(void) {
	int16_t cur_pitch, cur_roll, cur_yaw;
	int16_t dest_pitch, dest_roll, dest_yaw, base_throttle;
	static int16_t set_yaw = 0;
	uint8_t co_right = rx_co_right, cy_right = rx_cy_right,
		cy_front = rx_cy_front;
	/* Motors (top view):
	 * (A)_   .    _(B)
	 *    '#_ .  _#'
	 *      '#__#'
	 * - - - _##_ - - - - pitch axis
	 *     _#'. '#_
	 *   _#'  .   '#_
	 * (C)    .     (D)
	 *        |
	 *        '--- roll axis
	 */
	int32_t a, b, c, d;

	cli();
	cur_pitch = (ahrs_pitch >> 16) + (ahrs_pitch_rate >> 2);
	cur_roll = (ahrs_roll >> 16) + (ahrs_roll_rate >> 2);
	cur_yaw = ahrs_yaw + (ahrs_yaw_rate << 1);
	sei();

	if (modes & (1 << MODE_PANTILT_ENABLE)) {
		co_right = 0x80;
		cy_right = 0x80;
		cy_front = 0x80;
	}

	dest_pitch = ((int16_t) cy_front << 5) - (128 << 5);
	dest_roll = ((int16_t) cy_right << 5) - (128 << 5);
	set_yaw += ((int16_t) co_right << 2) - (128 << 2);
	dest_yaw = set_yaw;

	base_throttle = rx_co_throttle << 7;

	dest_pitch = -(cur_pitch + dest_pitch) / 1;
	dest_roll = -(cur_roll + dest_roll) / 1;
	dest_yaw = cur_yaw - dest_yaw;

	/* Some easing */
	if (dest_pitch < 0x400 && dest_pitch > -0x400)
		dest_pitch >>= 2;
	else if (dest_pitch > 0)
		dest_pitch -= 0x300;
	else
		dest_pitch += 0x300;
	if (dest_roll < 0x400 && dest_roll > -0x400)
		dest_roll >>= 2;
	else if (dest_roll > 0)
		dest_roll -= 0x300;
	else
		dest_roll += 0x300;

#define CLAMP(x, mi, ma)	\
	if (x < mi)		\
		x = mi;		\
	if (x > ma)		\
		x = ma;

	if (modes & (1 << MODE_HEADINGHOLD_ENABLE)) {
		CLAMP(dest_yaw, -0x800, 0x800)
	} else {
		dest_yaw = (128 << 5) - ((int16_t) co_right << 5);
		set_yaw = cur_yaw;
	}

	/* TODO: keep track of the motor feedback (through ahrs_pitch_rate
	 * and ahrs_roll_rate) for each motor and scale accordingly.
	 */

	a = (int32_t) base_throttle + dest_pitch + dest_roll + dest_yaw;
	b = (int32_t) base_throttle - dest_pitch + dest_roll - dest_yaw;
	c = (int32_t) base_throttle + dest_pitch - dest_roll - dest_yaw;
	d = (int32_t) base_throttle - dest_pitch - dest_roll + dest_yaw;
	CLAMP(a, 0, 32000);
	CLAMP(b, 0, 32000);
	CLAMP(c, 0, 32000);
	CLAMP(d, 0, 32000);
	actuator_set(0, (uint16_t) a);
	actuator_set(1, (uint16_t) b);
	actuator_set(2, (uint16_t) c);
	actuator_set(3, (uint16_t) d);
}

void loop(void) {
	my_delay(20); /* 50Hz update rate */

	modes_update();
	control_update();
#if 0
	serial_write_fp32(ahrs_pitch, ROLL_PITCH_180DEG / 180);
	serial_write_fp32(ahrs_roll, ROLL_PITCH_180DEG / 180);
	serial_write_fp32((int32_t) ahrs_yaw * 180, 32768);
	serial_write_eol();
#endif

	/* TODO: battery voltage check, send status over zigbee etc */
}

int main(void) {
	setup();

	for (;;)
		loop();

	return 0;
}
