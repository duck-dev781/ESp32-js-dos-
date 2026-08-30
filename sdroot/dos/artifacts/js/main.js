"use strict";


const ROOT =
    "/dos/artifacts/js";

const ROMS =
    ROOT + "/roms/";

const API =
    "/api";

const SAVE_API =
    API + "/save";


let currentPlayer =
    null;


const gameList =
    document.getElementById(
        "gameList"
    );

const statusElement =
    document.getElementById(
        "status"
    );

const playerScreen =
    document.getElementById(
        "playerScreen"
    );

const gamesScreen =
    document.getElementById(
        "games"
    );

const playerElement =
    document.getElementById(
        "player"
    );

const fileInput =
    document.getElementById(
        "fileInput"
    );


/* =========================================================
   STATUS
   ========================================================= */

function setStatus(text) {

    statusElement.textContent =
        text;
}


/* =========================================================
   SAVE KEY
   ========================================================= */

function saveKeyForBundle(filename) {

    /*
      js-dos normally creates a companion
      .changes file based on the bundle URL.

      We make the key explicitly stable so
      the ESP32 always knows which save belongs
      to which game.
    */

    return filename +
        ".changes";
}


/* =========================================================
   LOAD SAVE FROM ESP32
   ========================================================= */

async function pullSave(key) {

    try {

        const response =
            await fetch(
                SAVE_API +
                "?key=" +
                encodeURIComponent(key),
                {
                    cache: "no-store"
                }
            );


        /*
          404 simply means that this game
          does not have an SD save yet.
        */

        if (response.status === 404) {

            return null;
        }


        if (!response.ok) {

            throw new Error(
                "Save download HTTP " +
                response.status
            );
        }


        const buffer =
            await response.arrayBuffer();


        return new Uint8Array(
            buffer
        );

    }

    catch (error) {

        console.warn(
            "Could not load SD save:",
            error
        );

        return null;
    }
}


/* =========================================================
   SAVE TO ESP32
   ========================================================= */

async function pushSave(
    key,
    data
) {

    const formData =
        new FormData();


    const blob =
        new Blob(
            [data],
            {
                type:
                    "application/octet-stream"
            }
        );


    formData.append(
        "file",
        blob,
        key
    );


    const response =
        await fetch(
            SAVE_API +
            "?key=" +
            encodeURIComponent(key),
            {
                method: "POST",
                body: formData
            }
        );


    let result = {};


    try {

        result =
            await response.json();

    }

    catch (_) {

        /*
          ESP32 may return a non-JSON
          response if something goes wrong.
        */
    }


    if (
        !response.ok ||
        result.ok === false
    ) {

        throw new Error(
            result.message ||
            "SD save upload failed " +
            "(HTTP " +
            response.status +
            ")"
        );
    }


    return data;
}


/* =========================================================
   DELETE SAVE
   ========================================================= */

async function deleteSave(key) {

    const response =
        await fetch(
            SAVE_API +
            "?key=" +
            encodeURIComponent(key),
            {
                method:
                    "DELETE"
            }
        );


    if (response.status === 404) {

        return;
    }


    if (!response.ok) {

        throw new Error(
            "SD save delete HTTP " +
            response.status
        );
    }
}


/* =========================================================
   JS-DOS FS CHANGES
   ========================================================= */

function makeFsChanges(key) {

    return {

        /*
          Keep js-dos's normal save system enabled.
        */

        local: true,


        /*
          js-dos calls this when it needs
          the saved filesystem changes.

          key is supplied by js-dos.
        */

        pull: async function(
            jsDosKey
        ) {

            /*
              We use our stable key instead of
              trusting a browser-generated key.
            */

            return await pullSave(
                key
            );
        },


        /*
          js-dos calls this when the user
          presses its save/disk button or
          otherwise persists filesystem changes.

          Signature:

            push(key, data)
        */

        push: async function(
            jsDosKey,
            data
        ) {

            setStatus(
                "Saving to SD..."
            );


            try {

                const saved =
                    await pushSave(
                        key,
                        data
                    );


                setStatus(
                    "Saved to SD"
                );


                return saved;

            }

            catch (error) {

                console.error(
                    "Could not save to SD:",
                    error
                );


                setStatus(
                    "SD save failed"
                );


                throw error;
            }
        },


        /*
          Delete the persistent save
          from the ESP32 SD card.
        */

        delete: async function(
            jsDosKey
        ) {

            try {

                await deleteSave(
                    key
                );

            }

            catch (error) {

                console.warn(
                    "Could not delete SD save:",
                    error
                );
            }
        },


        /*
          Tell js-dos which key to use
          for this bundle.
        */

        urlToKey: async function(
            url
        ) {

            return key;
        }
    };
}


/* =========================================================
   LOAD GAMES
   ========================================================= */

async function loadGames() {

    setStatus(
        "Loading games..."
    );


    gameList.innerHTML =
        "Loading...";


    try {

        const response =
            await fetch(
                API + "/roms",
                {
                    cache:
                        "no-store"
                }
            );


        if (!response.ok) {

            throw new Error(
                "ROM list HTTP " +
                response.status
            );
        }


        const games =
            await response.json();


        gameList.innerHTML =
            "";


        if (
            !Array.isArray(games) ||
            games.length === 0
        ) {

            gameList.innerHTML =
                "<p>No .jsdos bundles found.</p>";


            setStatus(
                "Ready"
            );


            return;
        }


        for (
            const filename of games
        ) {

            if (
                typeof filename !==
                    "string" ||

                !filename ||

                !filename
                    .toLowerCase()
                    .endsWith(
                        ".jsdos"
                    )
            ) {

                continue;
            }


            createGameEntry(
                filename
            );
        }


        setStatus(
            games.length +
            " game(s)"
        );

    }

    catch (error) {

        console.error(
            error
        );


        gameList.innerHTML =
            "<p>Could not load games.</p>" +
            "<p>" +
            escapeHTML(
                error.message
            ) +
            "</p>";


        setStatus(
            "Error"
        );
    }
}


/* =========================================================
   GAME ENTRY
   ========================================================= */

function createGameEntry(
    filename
) {

    const row =
        document.createElement(
            "div"
        );


    row.className =
        "game";


    const name =
        document.createElement(
            "span"
        );


    name.className =
        "gameName";


    name.textContent =
        filename;


    const play =
        document.createElement(
            "button"
        );


    play.textContent =
        "Play";


    play.onclick =
        function() {

            playBundle(
                filename
            );
        };


    const del =
        document.createElement(
            "button"
        );


    del.textContent =
        "Delete";


    del.onclick =
        function() {

            deleteBundle(
                filename
            );
        };


    row.appendChild(
        name
    );

    row.appendChild(
        play
    );

    row.appendChild(
        del
    );


    gameList.appendChild(
        row
    );
}


/* =========================================================
   PLAY JS-DOS 8 BUNDLE
   ========================================================= */

function playBundle(
    filename
) {

    /*
      Make sure js-dos loaded.
    */

    if (
        typeof window.Dos !==
        "function"
    ) {

        alert(
            "js-dos was not loaded.\n\n" +
            "Check:\n" +
            "/dos/artifacts/js/js-dos/js-dos.js"
        );


        setStatus(
            "js-dos runtime missing"
        );


        return;
    }


    /*
      Close previous player.
    */

    if (currentPlayer) {

        try {

            if (
                typeof currentPlayer.stop ===
                "function"
            ) {

                currentPlayer.stop();
            }

        }

        catch (error) {

            console.warn(
                "Could not stop previous player:",
                error
            );
        }


        currentPlayer =
            null;
    }


    /*
      Clear old emulator.
    */

    playerElement.innerHTML =
        "";


    gamesScreen.hidden =
        true;

    playerScreen.hidden =
        false;


    /*
      URL of the .jsdos bundle.
    */

    const bundleURL =
        ROMS +
        encodeURIComponent(
            filename
        );


    /*
      Stable save key.

      Example:

        doom.jsdos.changes

      is stored on the SD card as:

        /dos/artifacts/js/roms/saves/
        doom.jsdos.changes
    */

    const saveKey =
        saveKeyForBundle(
            filename
        );


    console.log(
        "js-dos bundle:",
        bundleURL
    );


    console.log(
        "js-dos SD save key:",
        saveKey
    );


    /*
      Start js-dos.

      IMPORTANT:
      This keeps the same working
      Dos(element, options) pattern
      that your current version uses.
    */

    try {

        currentPlayer =
            window.Dos(
                playerElement,
                {

                    /*
                      Bundle URL.
                    */

                    url:
                        bundleURL,


                    /*
                      ESP32 SD-card save system.

                      js-dos v8 calls:
                        pull(key)
                        push(key, data)
                        delete(key)

                      for filesystem-change persistence.
                    */

                    fsChanges:
                        makeFsChanges(
                            saveKey
                        ),


                    /*
                      Emulator files.
                    */

                    pathPrefix:
                        ROOT +
                        "/js-dos/emulators/",


                    /*
                      Start automatically.
                    */

                    autoStart:
                        true,


                    /*
                      Speed up DOS boot.
                    */

                    fastForwardOnBoot:
                        true,


                    /*
                      js-dos events.
                    */

                    onEvent:
                        function(
                            event,
                            arg
                        ) {

                            console.log(
                                "js-dos event:",
                                event,
                                arg
                            );


                            if (
                                event ===
                                "ci-ready"
                            ) {

                                window.jsdosCI =
                                    arg;


                                console.log(
                                    "Command interface ready"
                                );


                                setStatus(
                                    "Running " +
                                    filename
                                );
                            }


                            if (
                                event ===
                                "error"
                            ) {

                                console.error(
                                    "js-dos error:",
                                    arg
                                );
                            }
                        }
                }
            );


        setStatus(
            "Starting " +
            filename +
            "..."
        );

    }

    catch (error) {

        console.error(
            "Could not start js-dos:",
            error
        );


        playerElement.innerHTML =
            "<div class='error'>" +

            "<h2>Cannot play bundle</h2>" +

            "<p>" +

            escapeHTML(
                error.message ||
                String(error)
            ) +

            "</p>" +

            "<p>Bundle:</p>" +

            "<code>" +

            escapeHTML(
                bundleURL
            ) +

            "</code>" +

            "</div>";


        setStatus(
            "Bundle error"
        );
    }
}


/* =========================================================
   DELETE BUNDLE
   ========================================================= */

async function deleteBundle(
    filename
) {

    if (
        !confirm(
            "Delete " +
            filename +
            "?"
        )
    ) {

        return;
    }


    try {

        const response =
            await fetch(
                API +
                "/delete?name=" +
                encodeURIComponent(
                    filename
                ),
                {
                    method:
                        "POST"
                }
            );


        const result =
            await response.json();


        if (
            !response.ok ||
            !result.ok
        ) {

            throw new Error(
                result.message ||
                "Delete failed"
            );
        }


        await loadGames();

    }

    catch (error) {

        alert(
            "Delete failed:\n" +
            error.message
        );
    }
}


/* =========================================================
   UPLOAD
   ========================================================= */

async function uploadBundle() {

    if (
        !fileInput.files ||
        fileInput.files.length === 0
    ) {

        alert(
            "Choose a .jsdos bundle first."
        );


        return;
    }


    const file =
        fileInput.files[0];


    if (
        !file.name
            .toLowerCase()
            .endsWith(
                ".jsdos"
            )
    ) {

        alert(
            "Only .jsdos bundles are allowed."
        );


        return;
    }


    const formData =
        new FormData();


    formData.append(
        "file",
        file,
        file.name
    );


    setStatus(
        "Uploading..."
    );


    try {

        const response =
            await fetch(
                API + "/upload",
                {
                    method:
                        "POST",
                    body:
                        formData
                }
            );


        const result =
            await response.json();


        if (
            !response.ok ||
            !result.ok
        ) {

            throw new Error(
                result.message ||
                "Upload failed"
            );
        }


        fileInput.value =
            "";


        setStatus(
            "Upload complete"
        );


        await loadGames();

    }

    catch (error) {

        console.error(
            error
        );


        setStatus(
            "Upload failed"
        );


        alert(
            "Upload failed:\n" +
            error.message
        );
    }
}


/* =========================================================
   BACK
   ========================================================= */

function goBack() {

    if (currentPlayer) {

        try {

            if (
                typeof currentPlayer.stop ===
                "function"
            ) {

                currentPlayer.stop();
            }

        }

        catch (error) {

            console.warn(
                error
            );
        }


        currentPlayer =
            null;
    }


    playerElement.innerHTML =
        "";


    playerScreen.hidden =
        true;

    gamesScreen.hidden =
        false;


    setStatus(
        "Ready"
    );


    loadGames();
}


/* =========================================================
   HTML ESCAPE
   ========================================================= */

function escapeHTML(
    value
) {

    return String(value)
        .replace(
            /[&<>"']/g,
            function(
                character
            ) {

                const entities = {

                    "&":
                        "&amp;",

                    "<":
                        "&lt;",

                    ">":
                        "&gt;",

                    "\"":
                        "&quot;",

                    "'":
                        "&#39;"
                };


                return entities[
                    character
                ];
            }
        );
}


/* =========================================================
   BUTTONS
   ========================================================= */

document
    .getElementById(
        "refreshButton"
    )
    .addEventListener(
        "click",
        loadGames
    );


document
    .getElementById(
        "uploadButton"
    )
    .addEventListener(
        "click",
        uploadBundle
    );


document
    .getElementById(
        "backButton"
    )
    .addEventListener(
        "click",
        goBack
    );


/* =========================================================
   START
   ========================================================= */

loadGames();
