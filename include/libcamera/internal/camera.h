/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2021, Google Inc.
 *
 * camera.h - Camera private data
 */
#ifndef __LIBCAMERA_INTERNAL_CAMERA_H__
#define __LIBCAMERA_INTERNAL_CAMERA_H__

#include <atomic>
#include <memory>
#include <set>
#include <string>

#include <libcamera/base/class.h>

#include <libcamera/camera.h>

namespace libcamera {

class PipelineHandler;
class Stream;

class Camera::Private : public Extensible::Private
{
	LIBCAMERA_DECLARE_PUBLIC(Camera)

public:
	Private(PipelineHandler *pipe, const std::string &id,
		const std::set<Stream *> &streams);
	~Private();

private:
	enum State {
		CameraAvailable,
		CameraAcquired,
		CameraConfigured,
		CameraStopping,
		CameraRunning,
	};

	bool isRunning() const;
	int isAccessAllowed(State state, bool allowDisconnected = false,
			    const char *from = __builtin_FUNCTION()) const;
	int isAccessAllowed(State low, State high,
			    bool allowDisconnected = false,
			    const char *from = __builtin_FUNCTION()) const;

	void disconnect();
	void setState(State state);

	std::shared_ptr<PipelineHandler> pipe_;
	std::string id_;
	std::set<Stream *> streams_;
	std::set<const Stream *> activeStreams_;

	bool disconnected_;
	std::atomic<State> state_;
};

} /* namespace libcamera */

#endif /* __LIBCAMERA_INTERNAL_CAMERA_H__ */
