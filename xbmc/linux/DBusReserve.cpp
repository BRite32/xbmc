/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
#include "system.h"
#include "DBusReserve.h"

#include <dbus/dbus.h>
#include <climits>
#include <algorithm>

#include "utils/log.h"
#include "DBusUtil.h"

/* This implements the code to exclusively acquire                  *
 * a device on the system describe at:                              *
 * http://git.0pointer.de/?p=reserve.git;a=blob_plain;f=reserve.txt */

CDBusReserve::CDBusReserve()
{
  CDBusError error;
  
  m_conn.Connect(DBUS_BUS_SESSION);
}

CDBusReserve::~CDBusReserve()
{
  while(m_devs.begin() != m_devs.end())
  {
    std::string buf = *m_devs.begin();
    ReleaseDevice(buf);
  }
}

bool CDBusReserve::AcquireDevice(const std::string& device)
{
  DBusMessagePtr msg, reply;
  DBusMessageIter args;
  CDBusError error;
  int res;

  // currently only max prio is supported since 
  // we don't implement the RequestRelease interface
  int prio = INT_MAX;

  std::string service = "org.freedesktop.ReserveDevice1." + device;
  std::string object  = "/org/freedesktop/ReserveDevice1/" + device;
  const char * interface = "org.freedesktop.ReserveDevice1";

  if (!m_conn)
    return false;

  res = dbus_bus_request_name(m_conn, service.c_str()
                                  , DBUS_NAME_FLAG_DO_NOT_QUEUE | (prio == INT_MAX ? 0 : DBUS_NAME_FLAG_ALLOW_REPLACEMENT)
                                  , error);
  if(res == -1)
  {
    CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): Request name failed: %s - %s", device.c_str(), error.Name().c_str(), error.Message().c_str());
    return false;
  }
  else if(res == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
  {
    CLog::Log(LOGDEBUG, "CDBusReserve::AcquireDevice(%s): Request name succeeded", device.c_str());
    m_devs.push_back(device);
    return true;
  }
  else if(res != DBUS_REQUEST_NAME_REPLY_EXISTS)
  {
    CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): Request name returned unknown code %d", device.c_str(), res);
    return false;
  }

  msg.reset(dbus_message_new_method_call(service.c_str(), object.c_str(), interface, "RequestRelease"));
  if (!msg)
  {
    CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): Failed to get function", device.c_str());
    return false;
  }

  dbus_message_iter_init_append(msg.get(), &args);
  if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &prio))
  {
    CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): Failed to append arguments", device.c_str());
    return false;
  }

  reply.reset(dbus_connection_send_with_reply_and_block(m_conn, msg.get(), 5000, error));
  if(!reply)
  {
    if(error.Name() == DBUS_ERROR_TIMED_OUT
    || error.Name() == DBUS_ERROR_UNKNOWN_METHOD
	  || error.Name() == DBUS_ERROR_NO_REPLY)
      CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): RequestRelease was denied on call", device.c_str());
    else
      CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): RequestRelease call failed: %s - %s", device.c_str(), error.Name().c_str(), error.Message().c_str());

    return false;
  }

  dbus_bool_t allowed;
  if(!dbus_message_get_args(reply.get(), error, DBUS_TYPE_BOOLEAN, &allowed, DBUS_TYPE_INVALID))
  {
    CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): Failed to get reply arguments", device.c_str());
    return false;
  }

  if(!allowed)
  {
    CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): RequestRelease was denied", device.c_str());
    return false;
  }

  res = dbus_bus_request_name(m_conn, service.c_str()
                                  , DBUS_NAME_FLAG_DO_NOT_QUEUE 
                                  | (prio == INT_MAX ? 0 : DBUS_NAME_FLAG_ALLOW_REPLACEMENT)
                                  | DBUS_NAME_FLAG_REPLACE_EXISTING
                                  , error);
  if(res == -1)
  {
    CLog::Log(LOGERROR, "CDBusReserve::AcquireDevice(%s): Request name failed after release: %s - %s", device.c_str(), error.Name().c_str(), error.Message().c_str());
    return false;
  }

  m_devs.push_back(device);
  CLog::Log(LOGDEBUG, "CDBusReserve::AcquireDevice(%s): Successfully reserved audio", device.c_str());
  return true;
}


bool CDBusReserve::ReleaseDevice(const std::string& device)
{
  std::vector<std::string>::iterator it = find(m_devs.begin(), m_devs.end(), device);
  if(it == m_devs.end())
  {
    CLog::Log(LOGDEBUG, "CDBusReserve::ReleaseDevice(%s): device wasn't acquired here", device.c_str());
    return false;
  }
  m_devs.erase(it);

  std::string service = "org.freedesktop.ReserveDevice1." + device;

  int res = dbus_bus_release_name(m_conn, service.c_str(), nullptr);
  if(res == DBUS_RELEASE_NAME_REPLY_RELEASED)
    CLog::Log(LOGDEBUG, "CDBusReserve::ReleaseDevice(%s): Released", device.c_str());
  else if(res == DBUS_RELEASE_NAME_REPLY_NON_EXISTENT)
    CLog::Log(LOGDEBUG, "CDBusReserve::ReleaseDevice(%s): Name didn't exist", device.c_str());
  else
    CLog::Log(LOGERROR, "CDBusReserve::ReleaseDevice(%s): Release failed", device.c_str());

  return res == DBUS_RELEASE_NAME_REPLY_RELEASED;
}
