/*
 * Copyright (c) 2021, Adam Boardman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef INPUT_DETECT_H
#define INPUT_DETECT_H

class Input_detect {

public:
	virtual bool get_current_state() const = 0;
};

#endif //INPUT_DETECT_H
