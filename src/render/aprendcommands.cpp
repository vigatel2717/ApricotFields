
#include "aprend_internal.hpp"

extern "C" {

aprend_command_list aprend_command_list_create() {
	aprend_command_list_t *result = (aprend_command_list_t *)malloc(sizeof(aprend_command_list_t));
	if (result)
		result = new (result) aprend_command_list_t();
	return result;
}
void aprend_command_list_destroy(aprend_command_list cmd_list) {
	if (cmd_list) {
		cmd_list->~aprend_command_list_t();
		free(cmd_list);
	}
}

void aprend_command_list_reset(aprend_command_list cmd_list) {
    if (!cmd_list) return;
    cmd_list->_commands.clear();
}
void aprend_send_command(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd) {
	if (!cmd_list || cmd._type == APREND_COMMAND_NONE)
		return;
    cmd_list->_commands.push_back(cmd);
}
}
