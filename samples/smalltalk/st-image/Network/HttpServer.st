HttpServer := Object [
    | listener running |

    serveOn: port [
        | sinceCollection |
        listener := Socket listenOn: port.
        listener timeoutMilliseconds: 30000.
        running := true.
        sinceCollection := 0.
        Transcript nextPutAll: 'HTTP listening on 127.0.0.1:'; show: listener localPort; lf.

        [
            [running] whileTrue: [
                [[
                    self startConnection: listener accept.
                    sinceCollection := sinceCollection + 1
                ] on: SocketTimeout do: [:error | ObjectMemory collect]]
                    on: SocketInterrupted do: [:error |
                        running ifTrue: [error signal]
                    ].

                sinceCollection >= 64 ifTrue: [
                    ObjectMemory collect.
                    sinceCollection := 0
                ]
            ]
        ] ensure: [listener close].

        Fiber run.
        ObjectMemory collect.
        ^self
    ]

    stop [
        running := false.
        listener interrupt.
        ^self
    ]

    startConnection: connection [
        connection timeoutMilliseconds: 5000.
        (Fiber fork: [self handleConnection: connection]) detach.
        ^self
    ]

    handleConnection: connection [
        [
            [
                [self respondOn: connection]
                    on: HttpError
                    do: [:error |
                        (HttpResponse status: error status reason: error messageText body: error messageText)
                            writeTo: connection headOnly: false
                    ]
            ] on: SocketError do: [:error | nil]
        ] ensure: [connection close].
        ^self
    ]

    respondOn: connection [
        | request response |
        request := HttpRequest readFrom: connection.
        request isNil ifTrue: [^self].

        (request method = 'GET' or: [request method = 'HEAD']) ifTrue: [
            response := self responseFor: request
        ] ifFalse: [
            response := HttpResponse status: 405 reason: 'Method Not Allowed' body: 'Method Not Allowed'
        ].
        response writeTo: connection headOnly: request method = 'HEAD'.
        ^self
    ]

    responseFor: request [
        request path = '/' ifTrue: [
            ^HttpResponse status: 200 reason: 'OK' body: 'Hello from Anvil Smalltalk fibers!'
        ].
        request path = '/health' ifTrue: [
            ^HttpResponse status: 200 reason: 'OK' body: 'ok'
        ].
        ^HttpResponse status: 404 reason: 'Not Found' body: 'Not Found'
    ]
]
