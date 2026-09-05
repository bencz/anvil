SocketReadStream := Stream [
    | socket buffer position limit consumed maximum |

    class on: connection limit: maximumBytes [
        ^self new initializeOn: connection limit: maximumBytes
    ]

    initializeOn: connection limit: maximumBytes [
        socket := connection.
        buffer := ByteArray new: 4096.
        position := 1.
        limit := 0.
        consumed := 0.
        maximum := maximumBytes.
        ^self
    ]

    nextByte [
        | value |
        consumed >= maximum ifTrue: [^HttpError signalStatus: 431 message: 'Request Header Fields Too Large'].

        position > limit ifTrue: [
            limit := socket receiveInto: buffer startingAt: 1 count: buffer size.
            position := 1.
            limit = 0 ifTrue: [^-1]
        ].

        value := buffer at: position.
        position := position + 1.
        consumed := consumed + 1.
        ^value
    ]

    nextHttpLine [
        | output byte |
        output := WriteStream on: ''.

        [true] whileTrue: [
            byte := self nextByte.
            byte = -1 ifTrue: [
                output size = 0 ifTrue: [^nil].
                ^HttpError signalStatus: 400 message: 'Bad Request'
            ].
            byte = 13 ifTrue: [
                self nextByte = 10 ifFalse: [^HttpError signalStatus: 400 message: 'Bad Request'].
                ^output contents
            ].
            (byte = 10 or: [byte = 0]) ifTrue: [^HttpError signalStatus: 400 message: 'Bad Request'].
            output size >= 8192 ifTrue: [^HttpError signalStatus: 431 message: 'Request Header Fields Too Large'].
            output nextPut: (Character value: byte)
        ]
    ]
]
