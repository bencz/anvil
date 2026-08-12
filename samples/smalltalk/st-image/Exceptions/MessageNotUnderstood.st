MessageNotUnderstood := Error [

    | receiver message |

    receiver [
        ^receiver
    ]

    receiver: anObject [
        receiver := anObject.
        ^self
    ]

    message [
        ^message
    ]

    message: aMessage [
        message := aMessage.
        ^self
    ]
]
