HttpRequest := Object [
    | method target version headers |

    class readFrom: socket [
        | input line request |
        input := SocketReadStream on: socket limit: 16384.
        line := input nextHttpLine.
        line isNil ifTrue: [^nil].
        request := self new parseRequestLine: line.
        request readHeadersFrom: input.
        ^request
    ]

    class isToken: text [
        | value |
        text isEmpty ifTrue: [^false].

        text do: [:character |
            value := character value.
            ((value >= 48 and: [value <= 57]) or: [
                (value >= 65 and: [value <= 90]) or: [
                    (value >= 97 and: [value <= 122]) or: [
                        '!#$%&''*+-.^_`|~' includes: character
                    ]
                ]
            ]) ifFalse: [^false]
        ].

        ^true
    ]

    parseRequestLine: line [
        | first second |
        first := line indexOf: Character space ifAbsent: [0].
        first <= 1 ifTrue: [^HttpError signalStatus: 400 message: 'Bad Request'].
        second := line indexOf: Character space startingAt: first + 1 ifAbsent: [0].
        second <= (first + 1) ifTrue: [^HttpError signalStatus: 400 message: 'Bad Request'].
        method := line copyFrom: 1 to: first - 1.
        target := line copyFrom: first + 1 to: second - 1.
        version := line copyFrom: second + 1 to: line size.

        (self class isToken: method) ifFalse: [^HttpError signalStatus: 400 message: 'Bad Request'].
        (version = 'HTTP/1.1' or: [version = 'HTTP/1.0']) ifFalse: [^HttpError signalStatus: 505 message: 'HTTP Version Not Supported'].
        (target startsWith: '/') ifFalse: [^HttpError signalStatus: 400 message: 'Bad Request'].
        target do: [:character |
            (character value <= 32 or: [character value >= 127 or: [character = $#]]) ifTrue: [
                ^HttpError signalStatus: 400 message: 'Bad Request'
            ]
        ].
        headers := Dictionary new.
        ^self
    ]

    readHeadersFrom: input [
        | line count |
        count := 0.
        line := input nextHttpLine.

        [line notNil and: [line notEmpty]] whileTrue: [
            count := count + 1.
            count > 100 ifTrue: [^HttpError signalStatus: 431 message: 'Request Header Fields Too Large'].
            self addHeaderLine: line.
            line := input nextHttpLine
        ].

        line isNil ifTrue: [^HttpError signalStatus: 400 message: 'Bad Request'].
        (version = 'HTTP/1.1' and: [(headers at: 'host' ifAbsent: ['']) isEmpty]) ifTrue: [
            ^HttpError signalStatus: 400 message: 'Bad Request'
        ].
        (headers includesKey: 'transfer-encoding') ifTrue: [^HttpError signalStatus: 501 message: 'Not Implemented'].
        (headers includesKey: 'content-length') ifTrue: [self validateContentLength].
        ^self
    ]

    addHeaderLine: line [
        | colon name value |
        colon := line indexOf: $: ifAbsent: [0].
        colon <= 1 ifTrue: [^HttpError signalStatus: 400 message: 'Bad Request'].
        name := line copyFrom: 1 to: colon - 1.
        (self class isToken: name) ifFalse: [^HttpError signalStatus: 400 message: 'Bad Request'].
        name := name asAsciiLowercase.
        value := (line copyFrom: colon + 1 to: line size) trimHttpWhitespace.

        value do: [:character |
            ((character value < 32 and: [character value ~= 9]) or: [character value = 127]) ifTrue: [
                ^HttpError signalStatus: 400 message: 'Bad Request'
            ]
        ].

        (headers includesKey: name) ifTrue: [
            (name = 'host' or: [name = 'content-length' or: [name = 'transfer-encoding']]) ifTrue: [
                ^HttpError signalStatus: 400 message: 'Bad Request'
            ].
            value := (headers at: name), ', ', value
        ].
        headers at: name put: value.
        ^self
    ]

    validateContentLength [
        | value nonzero |
        value := headers at: 'content-length'.
        value isEmpty ifTrue: [^HttpError signalStatus: 400 message: 'Bad Request'].
        nonzero := false.

        value do: [:character |
            (character value < 48 or: [character value > 57]) ifTrue: [^HttpError signalStatus: 400 message: 'Bad Request'].
            character value ~= 48 ifTrue: [nonzero := true]
        ].

        nonzero ifTrue: [^HttpError signalStatus: 413 message: 'Content Too Large'].
        ^self
    ]

    method [
        ^method
    ]

    target [
        ^target
    ]

    path [
        | question |
        question := target indexOf: $? ifAbsent: [target size + 1].
        ^target copyFrom: 1 to: question - 1
    ]

    headerAt: name ifAbsent: block [
        ^headers at: name asAsciiLowercase ifAbsent: block
    ]
]
