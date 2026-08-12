Exception := Object [

    | messageText |

    class signal [
        ^self new signal
    ]

    class signal: aString [
        ^(self new messageText: aString) signal
    ]

    messageText [
        ^messageText
    ]

    messageText: aString [
        messageText := aString.
        ^self
    ]

    signal [
        <primitive: ExceptionSignal>
    ]
]
