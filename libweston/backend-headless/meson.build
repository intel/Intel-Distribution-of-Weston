if not get_option('backend-headless')
	subdir_done()
endif

config_h.set('BUILD_HEADLESS_COMPOSITOR', '1')

srcs_headless = [
	'headless.c',
	presentation_time_server_protocol_h,
]

if get_option('renderer-gl')
	dep_gbm = dependency('gbm', required: false)
	if dep_gbm.found()
	      config_h.set('BUILD_HEADLESS_GBM', '1')
	endif
endif

if get_option('remoting')
	if dep_gbm.found()
	      srcs_headless += 'headless-virtual.c'
	      config_h.set('BUILD_HEADLESS_VIRTUAL', '1')
	endif
endif

plugin_headless = shared_library(
	'headless-backend',
	srcs_headless,
	include_directories: common_inc,
	dependencies: [ dep_libweston_private, dep_libdrm_headers, dep_libdl, dep_libdrm, dep_gbm],
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston,
)
env_modmap += 'headless-backend.so=@0@;'.format(plugin_headless.full_path())
install_headers(backend_headless_h, subdir: dir_include_libweston_install)
