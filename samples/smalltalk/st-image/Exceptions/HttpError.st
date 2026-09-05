HttpError := Error [
    | status |

    class signalStatus: code message: text [
        ^(self new status: code; messageText: text) signal
    ]

    status: code [
        status := code.
        ^self
    ]

    status [
        ^status
    ]
]
