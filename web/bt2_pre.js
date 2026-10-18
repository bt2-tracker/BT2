// Bernttracker II (web): runs inside the program's JavaScript before main().
// Mounts the user's own storage (IndexedDB) as /home/web_user so songs, samples,
// folders and settings are kept between sessions.

// the page reads the mixed sound through this (always the current memory view)
Module['bt2HeapF32'] = function () { return HEAPF32; };

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
	if (typeof document === 'undefined')
		return; // audio worklet / worker scopes: nothing to do

	Module['bt2FS'] = FS;
	FS.mount(IDBFS, {}, '/home/web_user');

	var stage = function (t) { if (globalThis.BT2 && globalThis.BT2.loadStage) globalThis.BT2.loadStage(t); };
	stage('Henter lagrede filer…');
	addRunDependency('bt2-storage');

	// Safari sometimes doesn't answer the first IndexedDB request after a page load until it
	// is "poked" again. Poke it until it answers, then load the stored files.
	var idbReady = function (done) {
		if (typeof indexedDB === 'undefined' || !indexedDB.databases) { done(); return; }
		var finished = false;
		var finish = function () { if (!finished) { finished = true; clearInterval(timer); done(); } };
		var poke = function () { try { indexedDB.databases().then(finish, finish); } catch (e) { finish(); } };
		var timer = setInterval(poke, 100);
		poke();
		setTimeout(finish, 3000);
	};

	idbReady(function () {
		FS.syncfs(true, function (err) {
			if (err)
				console.warn('Could not load stored files:', err);

			try { FS.chdir('/home/web_user'); } catch (e) {}
			stage('Starter…');
			removeRunDependency('bt2-storage');
		});
	});
});
