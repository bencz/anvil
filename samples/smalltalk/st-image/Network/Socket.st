Socket := Object [
    | handle timeout |

    class listenOn: port [
        ^self new initializeHandle: (self check: (self primitiveListen: port))
    ]

    class check: result [
        result = -7 ifTrue: [^SocketTimeout signal: 'Socket operation timed out'].
        result = -10 ifTrue: [^SocketInterrupted signal: 'Socket operation interrupted'].
        result < 0 ifTrue: [^SocketError signal: 'Socket operation failed with status ', result negated asString].
        ^result
    ]

    class primitiveListen: port [
        <primitive: SocketListenPrimitive>
        ^InvalidArgumentError signal: 'Port must be an integer between 0 and 65535'
    ]

    class primitiveAccept: listener timeout: milliseconds [
        <primitive: SocketAcceptPrimitive>
        ^InvalidArgumentError signal: 'Invalid accept arguments'
    ]

    class primitiveClose: identifier [
        <primitive: SocketClosePrimitive>
        ^InvalidArgumentError signal: 'Invalid socket handle'
    ]

    class primitivePort: identifier [
        <primitive: SocketPortPrimitive>
        ^InvalidArgumentError signal: 'Invalid socket handle'
    ]

    class primitiveInterrupt: identifier [
        <primitive: SocketInterruptPrimitive>
        ^InvalidArgumentError signal: 'Invalid socket handle'
    ]

    class primitiveReceive: identifier into: bytes startingAt: start count: count timeout: milliseconds [
        <primitive: SocketReceivePrimitive>
        ^InvalidArgumentError signal: 'Invalid receive buffer, range or timeout'
    ]

    class primitiveSend: identifier from: bytes startingAt: start count: count timeout: milliseconds [
        <primitive: SocketSendPrimitive>
        ^InvalidArgumentError signal: 'Invalid send buffer, range or timeout'
    ]

    initializeHandle: identifier [
        handle := identifier.
        timeout := 30000.
        ^self
    ]

    timeoutMilliseconds: milliseconds [
        ((milliseconds isKindOf: Integer) and: [milliseconds >= 0 and: [milliseconds <= 4294967295]]) ifFalse: [
            ^InvalidArgumentError signal: 'Invalid socket timeout'
        ].
        timeout := milliseconds.
        ^self
    ]

    localPort [
        ^self class check: (self class primitivePort: handle)
    ]

    accept [
        ^self class new initializeHandle: (self class check: (self class primitiveAccept: handle timeout: timeout))
    ]

    receiveInto: bytes startingAt: start count: count [
        ^self class check: (self class primitiveReceive: handle into: bytes startingAt: start count: count timeout: timeout)
    ]

    send: bytes startingAt: start count: count [
        ^self class check: (self class primitiveSend: handle from: bytes startingAt: start count: count timeout: timeout)
    ]

    sendAll: bytes [
        | position count |
        position := 1.

        [position <= bytes size] whileTrue: [
            count := self send: bytes startingAt: position count: bytes size - position + 1.
            count = 0 ifTrue: [^SocketError signal: 'Socket write made no progress'].
            position := position + count
        ].

        ^bytes
    ]

    close [
        handle isNil ifTrue: [^self].
        self class check: (self class primitiveClose: handle).
        handle := nil.
        ^self
    ]

    interrupt [
        self class check: (self class primitiveInterrupt: handle).
        ^self
    ]
]
