cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_project)

target_include_directories(app PRIVATE
     src
     src/init
    src/test/core
    src/test/core/unity
    src/test/param_manager
    src/vsc
    gen/vsc
    src/test/camyu
    src/core/protocol_parser
    src/port
    src/core/param_manager/core
    src/core/param_manager/config
    src/core/param_manager/storage
    src/protocols/ymodem/src
    src/protocols/ymodem/adapter
    src/protocols/imperx
    src/protocols/camyu
    src/core/file_process
    src/lib/flashdb/inc
    src/lib/flashdb/port/fal/inc
    src/lib/flashdb/config
    src/lib/lwevt/src/include
    src/lib/lwevt/user_config
    src/bsp/rp2350
    src/modules/app/auto_exp
    src/modules/ip/sensor
)

if(TEST_MODE)

  target_sources(app PRIVATE
      src/test/core/unified_test_main.c
      src/test/core/test_suite.c
      src/test/core/unity/unity.c
  )

  target_sources(app PRIVATE
      src/test/param_manager/test_main.c
      src/test/param_manager/test_common.c
      src/test/param_manager/test_read_write.c
      src/test/param_manager/test_boundary.c
      src/test/param_manager/test_init_order.c
      src/test/param_manager/test_error_handling.c
      src/test/param_manager/test_load_save.c
      src/test/param_manager/test_flush.c
      src/test/param_manager/test_reset.c
      src/test/param_manager/test_other.c
      src/test/param_manager/test_multi_bank.c
  )

  target_sources(app PRIVATE
      src/test/protocol_chain/test_chain.c
  )

  target_sources(app PRIVATE
      src/test/imperx/test_imperx.c
  )

  target_sources(app PRIVATE
      src/test/ymodem/unit/test_common.c
      src/test/ymodem/unit/test_sender.c
      src/test/ymodem/unit/test_receiver.c
  )

  target_sources(app PRIVATE
      src/test/camyu/test_camyu.c
  )

  target_sources(app PRIVATE
      src/test/vsc/test_vsc_resolver.c
      src/vsc/vsc_resolver.c
      src/vsc/vsc_feature.c
      src/vsc/vsc_loader.c
      src/modules/ip/crop/crop_driver.c
      src/modules/ip/crop/crop_vsc.c
      src/modules/ip/binning/binning_driver.c
      src/modules/ip/binning/binning_vsc.c
      src/modules/ip/decoder/decoder_driver.c
      src/modules/ip/decoder/decoder_vsc.c
      src/modules/ip/histogram/histogram_driver.c
      src/modules/ip/histogram/histogram_vsc.c
      src/modules/sensor/sensor_vsc.c
      src/modules/sensor/isc_bridge.c
      src/core/isc/isc_core.c
      src/core/isc/sensors/isc_sensor_imx477.c
  )

  target_sources(app PRIVATE
      src/core/protocol_parser/protocol_parser.c
      src/core/protocol_parser/protocol_chain.c
      src/core/param_manager/core/param_manager.c
      src/core/param_manager/core/app_param_manager.c
      src/core/param_manager/core/ip_param_manager.c
      src/core/param_manager/core/param_data_ops.c
      src/core/param_manager/core/param_dump.c
      src/port/port_impl.c
      src/protocols/ymodem/src/ymodem_common.c
      src/protocols/ymodem/src/ymodem_receiver.c
      src/protocols/ymodem/src/ymodem_sender.c
      src/protocols/imperx/protocol_parser_imperx.c
      src/protocols/imperx/imperx_cmd_map.c
      src/protocols/camyu/protocol_parser_camyu.c
  )

else()

  target_compile_definitions(app PRIVATE USE_FLASHDB)

  target_sources(app PRIVATE
      src/main.c
      src/init/init_thread.c
      src/init/protocol_chain_init.c
      src/init/param_manager_init.c
      src/init/sys_param.c
      src/init/file_storage_init.c
      src/init/protocol_parser_thread.c
      src/core/protocol_parser/protocol_chain.c
      src/core/protocol_parser/protocol_parser.c
      src/core/param_manager/core/param_manager.c
      src/core/param_manager/core/app_param_manager.c
      src/core/param_manager/core/ip_param_manager.c
      src/core/param_manager/core/param_data_ops.c
      src/core/param_manager/core/param_dump.c
      src/port/port_impl.c
      src/core/param_manager/storage/param_storage_flashdb.c
      src/bsp/rp2350/fal_flash_rp2350.c
      src/protocols/camyu/protocol_parser_camyu.c
      src/protocols/imperx/protocol_parser_imperx.c
      src/protocols/imperx/imperx_cmd_map.c
      src/protocols/ymodem/adapter/protocol_parser_ymodem.c
      src/protocols/ymodem/src/ymodem_common.c
      src/protocols/ymodem/src/ymodem_receiver.c
      src/protocols/ymodem/src/ymodem_sender.c
      src/core/file_process/file_processing.c
      src/core/file_process/file_io_fs.c
      src/core/file_process/file_io_flash.c
      src/lib/flashdb/src/fdb.c
      src/lib/flashdb/src/fdb_kvdb.c
      src/lib/flashdb/src/fdb_utils.c
      src/lib/flashdb/port/fal/src/fal.c
      src/lib/flashdb/port/fal/src/fal_flash.c
      src/lib/flashdb/port/fal/src/fal_partition.c
      src/lib/lwevt/src/lwevt/lwevt.c
      src/modules/app/auto_exp/ae_instance.c
      src/modules/app/auto_exp/auto_exp_control.c
      src/modules/ip/sensor/sensor_module.c
  )

endif()
