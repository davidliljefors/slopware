#pragma once

// Go-To-Text-In-File module: search within a single file with live cursor preview.

void plugin_goto_text_in_file_init();
void plugin_goto_text_in_file_tick();
void plugin_goto_text_in_file_shutdown();

// Set the file to search in. Reads content from disk. Call before showing.
void plugin_goto_text_in_file_set_file(const char* path);
