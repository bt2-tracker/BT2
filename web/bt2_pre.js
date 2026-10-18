// BerntTracker (web): runs inside the program's JavaScript before main().
// Mounts the user's own storage (IndexedDB) as /home/web_user so songs, samples,
// folders and settings are kept between sessions.

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
	if (typeof document === 'undefined')
		return; // audio worklet / worker scopes: nothing to do

	Module['bt2FS'] = FS;
	FS.mount(IDBFS, {}, '/home/web_user');

	addRunDependency('bt2-storage');
	FS.syncfs(true, function (err) {
		if (err)
			console.warn('Could not load stored files:', err);

		try { FS.chdir('/home/web_user'); } catch (e) {}
		removeRunDependency('bt2-storage');
	});
});
