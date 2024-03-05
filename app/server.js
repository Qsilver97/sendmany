const express = require('express');
const bodyParser = require('body-parser');
const app = express();
const http = require('http').createServer(app);
const io = require('socket.io')(http);
const { spawn } = require('child_process');

// Set the view engine to ejs
app.set('view engine', 'ejs');
// Serve static files from the 'public' directory
app.use(express.static('public'));
// Use body-parser to parse form data
app.use(bodyParser.urlencoded({ extended: true }));

let seedInfo = null;

app.get('/login', (req, res) => {
    res.render('login')
})

app.get('/', (req, res) => {
    res.render('index')
})

app.get('/dashboard', (req, res) => {
    res.render('dashboard', seedInfo)
})

app.get('/check', (req, res) => {
    res.render('check')
})

app.get('/create', (req, res) => {
    res.render('create', seedInfo)
})

app.post('/create', (req, res) => {
    seedInfo = {...req.body, result: JSON.parse(req.body.result)}
    console.log(seedInfo)
    res.redirect('create')
})

app.post('/check', (req, res) => {
    res.redirect('check')
})

app.post('/confirm', (req, res) => {
    const compare = seedInfo.result.display.split(' ').every((word, index) => {
        return req.body[`seed${index}`] === word;
    })
    console.log(compare)
    if(compare){
        res.redirect('dashboard')
    }else {
        res.redirect('check?status=nomatch')
    }
})

app.post('/dashboard', (req, res) => {
    seedInfo = {...req.body, result: JSON.parse(req.body.result)}
    res.redirect('dashboard')
})

app.post('/logout', (req, res) => {
    seedInfo = null
    res.redirect('login')
})

// Establish socket connection
io.on('connection', (socket) => {
    console.log('A user connected');
    let mainChild = null;
    let v1Child = null;

    // Function to kill child processes safely
    const killChildProcesses = () => {
        if (mainChild) {
            mainChild.kill();
            mainChild = null;
        }
        if (v1Child) {
            v1Child.kill();
            v1Child = null;
        }
    };

    // Handler for 'start' event
    socket.on('start', (msg) => {
        killChildProcesses();

        mainChild = spawn('node', ['./emscripten/command.js']);
        v1Child = spawn('node', ['./emscripten/v1request.js']);

        // Handle output from mainChild
        mainChild.stdout.on('data', (data) => {
            socket.emit('log', { value: data.toString(), flag: 'log' });
        });

        mainChild.stderr.on('data', (data) => {
            socket.emit('log', { value: `ERROR: ${data.toString()}`, flag: 'log' });
        });

        mainChild.on('close', (code) => {
            // socket.emit('log', `Child process exited with code ${code}`);
        });

    });

    // Handler for 'run' event
    socket.on('run', (msg) => {
        socket.broadcast.emit('qwallet', msg);
    });

    // Handler for 'stop' event
    socket.on('stop', () => {
        killChildProcesses();
    });

    // Handler for 'broadcast' event
    socket.on('broadcast', (message) => {
        socket.broadcast.emit(message.command, message.message);
    });

    // Handle disconnection
    socket.on('disconnect', () => {
        killChildProcesses();
    });
});

// Start the server
http.listen(3000, () => {
    console.log('Server listening on port 3000');
});
