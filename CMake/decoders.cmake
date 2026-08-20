#===============================================================================
#= C decoder DLL/SO build rules
#-------------------------------------------------------------------------------

set(C_DECODER_OUTPUT_DIR "${EXECUTABLE_OUTPUT_PATH}/decoders/c_decoders")

set(C_DECODERS spi_c i2c_c uart_c can_c jtag_c swd_c onewire_c i2s_c lin_c hdlc_c microwire_c mdio_c ps2_c dmx512_c nrzi_c ir_nec_c ir_rc5_c dcf77_c cec_c spdif_c usb_signalling_c 4b5b_c can_fd_c iso7816_c lpc_c dali_c c2_c graycode_c counter_c lm75_c ds1307_c ds3231_c numbers_and_state_c seven_segment_c pwm_c pwm_waveform_c wiegand_c ir_sirc_c edid_c i2c_packet_c i2cdemux_c i2cfilter_c ltc26x7_c ad5593r_c adxl345_c atsha204a_c bh1750_c eeprom24xx_c flexray_c mipi_rffe_c usb_power_delivery_c iebus_c spacewire_c qspi_c sdio_c spi_dual_quad_c uart_fast_c cjtag_c mlx90614_c mpu6050_c mxc6225xu_c nunchuk_c pca9571_c rtc8564_c ssd1306_c st25dv_c tcs3472x_c tpm_tis_i2c_c tmc_c sent_c sle44xx_c pjdl_c onewire_link_c ac97_c sdcard_sd_c emmc_sd_c swim_c rvswd_c a7105_c ad5626_c ad79x0_c ade77xx_c adf435x_c adb_c afsk_c am230x_c caliper_c carrera_c xfp_c hdcp_c hdmi_scdc_c tca6408a_c tmp102_c dcc_c delta_sigma_c dsi_c em4100_c em4305_c enc28j60_c ltc242x_c max6954_c max7219_c mrf24j40_c ieee488_c ir_irmp_c ir_ltto_c ir_rc6_c ir_recoil_c eth_an_c fsi_c gpib_c guess_bitrate_c iec_c adns5020_c as5047_c avr_isp_c cc1101_c cyrf6936_c mvb_c mcs48_c one_single_wire_c ook_c opentherm_c jitter_c lfast_c maple_bus_c miller_c morse_c nes_gamepad_c nrf24l01_c nrf905_c rfm12_c ssi32_c st25r39xx_spi_c sdcard_spi_c spiflash_c spi_tpm_c tpm_tis_spi_c x2444m_c rgb_led_spi_c sda2506_c signature_c sony_md_c st7735_c st7789_c parallel_c pcfx_ctrlr_c rinnai_control_panel_c rpm_c sae_j1850_vpw_c arm_itm_c arm_tpiu_c bluetooth_h4_c boost_c crsf_c bean_c ccd_c cjtag_oscan0_c rgb_led_ws281x_c stepper_motor_c j1708_c midi_c modbus_c pan1321_c pn532_c sbus_futaba_c scs_c ufcs_c amulet_ascii_c streletz_c z80_c adat_c arm_etmv3_c aud_c avr_pdi_c jtag_avr_c jtag_ejtag_c jtag_stm32_c mipi_dsi_c pxx1_c qi_c rc_encode_c sdq_c onewire_network_c ds2408_c ds243x_c ds28ea00_c eeprom93xx_c spi_fast_c swi_c t55xx_c tdm_audio_c tdm_audio_fast timing_c tlc5620_c xy2_100_c cfp_c ps2_keyboard_c ps2_mouse_c usb_packet_c usb_request_c avclan_c ethernet_c arp_c ipv4_c udp_c ook_oregon_c ook_vis_c ltar_smartdevice_c ir_ltto_decode_c sony_md_decode_c sipi_c pjon_c tpm_fifo_tis_c tm1637_c tm1638_c ltar_smartdevice_decode_c)

# Automatically find private decoders
file(GLOB PRIVATE_DECODERS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/c_decoders_private/*_c.c")
foreach(src_path ${PRIVATE_DECODERS_SRC})
	get_filename_component(dec_name ${src_path} NAME_WE)
	list(APPEND C_DECODERS ${dec_name})
endforeach()

foreach(dec ${C_DECODERS})
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/c_decoders_private/${dec}.c")
		set(DEC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/c_decoders_private/${dec}.c")
	else()
		set(DEC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/c_decoders/${dec}.c")
	endif()

	add_library(decoder_${dec} MODULE
		${DEC_SRC}
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/c_decoder_api.c
	)

	target_include_directories(decoder_${dec} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode
		${CMAKE_CURRENT_SOURCE_DIR}/common
		${GLIB_INCLUDE_DIRS}
		${Python3_INCLUDE_DIRS}
	)

	target_compile_definitions(decoder_${dec} PRIVATE
		SRD_C_DECODER_DLL
	)

	target_link_libraries(decoder_${dec}
		-lglib-2.0
	)

	set_target_properties(decoder_${dec} PROPERTIES
		OUTPUT_NAME ${dec}
		LIBRARY_OUTPUT_DIRECTORY ${C_DECODER_OUTPUT_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${C_DECODER_OUTPUT_DIR}
	)
endforeach()

#===============================================================================
#= irmp shared library
#-------------------------------------------------------------------------------

add_library(irmp SHARED
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/irmp/irmp-main-sharedlib.c
)

target_include_directories(irmp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/irmp
	${GLIB_INCLUDE_DIRS}
	${Python3_INCLUDE_DIRS}
)

target_compile_definitions(irmp PRIVATE
	IRMP_PROTOCOL_NAMES=1
)

if(WIN32)
	target_compile_definitions(irmp PRIVATE WIN32)
endif()

target_link_libraries(irmp
	Python3::Python
	-lglib-2.0
)

set_target_properties(irmp PROPERTIES
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}
	RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}
)

#===============================================================================
#= decoder_test - C decoder test harness
#-------------------------------------------------------------------------------

if(BUILD_DECODER_TEST)
	# Static library for libsigrokdecode so decoder_test can link against it.
	# The main PXView executable compiles these sources directly; this static
	# library exists solely to provide a linkable target for decoder_test.
	add_library(sigrokdecode_static STATIC
		${libsigrokdecode_SOURCES}
		${CMAKE_CURRENT_SOURCE_DIR}/common/log/xlog.c
	)

	target_include_directories(sigrokdecode_static PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode
		${CMAKE_CURRENT_SOURCE_DIR}/common
		${GLIB_INCLUDE_DIRS}
		${Python3_INCLUDE_DIRS}
	)

	target_link_libraries(sigrokdecode_static
		-lglib-2.0
		${PY_LIB}
		mimalloc
	)

	# decoder_test executable
	add_executable(decoder_test
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/tests/decoder_test.c
	)

	target_include_directories(decoder_test PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}                        # for <libsigrokdecode/libsigrokdecode.h>
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode        # for internal headers via sigrokdecode_static
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/tests  # for "cJSON.h"
		${GLIB_INCLUDE_DIRS}
		${Python3_INCLUDE_DIRS}                            # for libsigrokdecode-internal.h (Python.h)
	)

	# CJSON_IMPLEMENTATION is needed by the cJSON single-header library.
	# It is also defined in decoder_test.c itself; the CMake definition
	# ensures it is set regardless of source-level changes.
	target_compile_definitions(decoder_test PRIVATE
		CJSON_IMPLEMENTATION
	)

	target_link_libraries(decoder_test
		sigrokdecode_static
		-lglib-2.0
	)

	set_target_properties(decoder_test PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}
	)
endif()
