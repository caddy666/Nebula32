/*
 * Copyright (c) 2021, Adam Boardman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OUTPUT_SWITCH_H
#define OUTPUT_SWITCH_H

class Output_switch {

public:
	virtual void set_output_state(bool desired_state)=0;

	virtual void desired_state_achieved()=0;
};

#endif //OUTPUT_SWITCH_H
