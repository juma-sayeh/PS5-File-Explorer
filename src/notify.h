/*
 * File Explorer - PS5 notification helpers.
 */

#pragma once


int bfpilot_notify_send(const char *message, const char *submessage);
int bfpilot_notify_test(void);
void bfpilot_notify(const char *message, const char *submessage);
