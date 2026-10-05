'use strict';
const { contextBridge, ipcRenderer } = require('electron');
const subscribe = channel => callback => {
  const listener = (_event, value) => callback(value);
  ipcRenderer.on(channel, listener);
  return () => ipcRenderer.removeListener(channel, listener);
};
contextBridge.exposeInMainWorld('iframe', {
  connect: options => ipcRenderer.invoke('connect', options),
  disconnect: () => ipcRenderer.invoke('disconnect'),
  fullscreen: () => ipcRenderer.invoke('fullscreen'),
  hosts: () => ipcRenderer.invoke('hosts'),
  input: value => ipcRenderer.send('input', value),
  ack: (id, micros) => ipcRenderer.send('ack', id, micros),
  keyframe: () => ipcRenderer.send('keyframe'),
  onMessage: subscribe('stream-message'), onState: subscribe('state'), onHosts: subscribe('hosts'),
  onRtt: subscribe('rtt'), onReleaseInput: subscribe('release-input'), onDiscoveryWarning: subscribe('discovery-warning'),
  onMetaKey: subscribe('meta-key')
});
