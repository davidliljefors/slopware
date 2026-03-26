#pragma once

// Go-To-Text module for the VS plugin.

void plugin_goto_text_init();
void plugin_goto_text_tick();
void plugin_goto_text_shutdown();

// Cancel any in-flight background search and wait for it to finish.
void plugin_goto_text_cancel_search();
