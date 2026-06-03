const dgram = require('dgram');
const WebSocket = require('ws');

const wss = new WebSocket.Server({ port : 8080});

const udpServer = dgram.createSocket('udp4');

udpServer.on('message', (msg, rinfo) => {
    wss.clients.forEach((client) => {
        if(client.readyState === WebSocket.OPEN) {
            client.send(msg);
        }
    });
});

udpServer.on('listening', () => {
    const address = udpServer.address();
    console.log(`UDP server listening on ${address.address}:${address.port}`);
    console.log('WebSocket server listening on port 8080');
});

udpServer.bind(12345, '127.0.0.1');