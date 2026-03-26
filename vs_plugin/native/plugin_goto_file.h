#pragma once

// Go-To-File module for the VS plugin.

void plugin_goto_file_init();
void plugin_goto_file_tick();
void plugin_goto_file_shutdown();

// Cancel any in-flight background search and wait for it to finish.
void plugin_goto_file_cancel_search();
