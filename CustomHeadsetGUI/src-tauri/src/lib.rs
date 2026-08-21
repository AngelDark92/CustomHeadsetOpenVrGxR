mod driver_installer;
mod i18n;
mod js_api;
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .setup(|app| {
            if cfg!(debug_assertions) {
                app.handle().plugin(
                    tauri_plugin_log::Builder::default()
                        .level(log::LevelFilter::Info)
                        .build(),
                )?;
            }

            if cfg!(not(debug_assertions)) {
                i18n::load_i18n_page(app);
            }

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            js_api::get_executable_path,
            js_api::is_vrmonitor_running,
            js_api::restart_vrcompositor,
            js_api::kill_process,
            js_api::launch_process,
            driver_installer::install_driver_transactional,
            driver_installer::uninstall_driver_transactional,
            driver_installer::verify_driver_install,
            driver_installer::write_json_file_transactional,
            driver_installer::preview_driver_cleanup,
            driver_installer::execute_driver_cleanup,
            js_api::run_process_sync,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
