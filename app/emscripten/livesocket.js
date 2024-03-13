// Import necessary modules
const io = require('socket.io-client');
const axios = require('axios');
const { PORT } = require('../utils/constants');
const WebSocket = require('ws');

let startTime = performance.now()
let currentAddress = null;

// Connect to the socket server
const baseURL = `http://localhost:${PORT}`;
const liveSocketURL = 'ws://93.190.139.223:4444';

const socket = io(baseURL);
const liveSocket = new WebSocket(liveSocketURL);

// Event handler for successfully opening a connection
liveSocket.on('open', () => {
    console.log("Connected to the server");
});

liveSocket.on('error', (error) => {
    console.error(`WebSocket error: ${error}`);
});

liveSocket.onmessage = function(event) {
    console.log(event.data, 222, typeof event.data)
    if(event.data == "") {
        let endTime = performance.now();
        if(endTime > startTime + 60000) {
            liveSocket.send(currentAddress);
        }
    } else {
        startTime = performance.now()
        socket.emit('broadcast', { command: 'liveSocketResponse', message: event.data });
    }
}

liveSocket.on('close', () => {
    console.log("Disconnected from the server");
});

setInterval(() => {
    let endTime = performance.now();
    
    if(endTime > startTime + 60000) {
        liveSocket.send(currentAddress);
    }

}, 1000);

socket.on('liveSocketRequest', async (message) => {
    if(message.flag == "address") {
        currentAddress = message.data;
    }
    liveSocket.send(message.data);
})
