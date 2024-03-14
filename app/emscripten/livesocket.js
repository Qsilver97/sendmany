// Import necessary modules
const io = require('socket.io-client');
const axios = require('axios');
const { PORT } = require('../utils/constants');
const WebSocket = require('ws');

let addressStartTime = {}

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
    // console.log(event.data, 222, typeof event.data)
    if(event.data == "") {
        let endTime = performance.now();
        for(let address in addressStartTime) {
            if(endTime > addressStartTime[address] + 6000) {
                liveSocket.send(address)
            }
        }
    } else {
        // startTime = performance.now()
        try {
            let data = JSON.parse(event.data);
            if(data.address) {
                addressStartTime[data.address] = performance.now()
            }
        } catch (error) {
            
        }
        socket.emit('broadcast', { command: 'liveSocketResponse', message: event.data });
    }
}

liveSocket.on('close', () => {
    console.log("Disconnected from the server");
});

setInterval(() => {
    let endTime = performance.now();
    for(let address in addressStartTime) {
        if(endTime > addressStartTime[address] + 6000) {
            liveSocket.send(address)
        }
    }
}, 1000);

socket.on('liveSocketRequest', async (message) => {
    if(addressStartTime[message.data] && message.flag == "address"){
        console.log('Already sent this address')
    } else {
        if(message.data != "" && message.flag == "address"){
            console.log(message.data, "ssss")
            liveSocket.send(message.data);
        }
    }
    if(message.flag == "address" && message.data != "") {
        addressStartTime[message.data] = performance.now()
    }
})
