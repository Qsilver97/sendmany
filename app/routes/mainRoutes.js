const express = require('express');
const router = express.Router();
const mainController = require('../controllers/mainController');

router.get('/login', mainController.getLogin);
router.get('/', mainController.getCli);
router.get('/dashboard', mainController.getDashboard);
router.get('/check', mainController.getCheck);
router.get('/create', mainController.getCreate);

router.post('/check', mainController.postCheck);
router.post('/create', mainController.postCreate);
router.post('/addaccount', mainController.postAddAccount);
router.post('/confirm', mainController.postConfirm);
router.post('/dashboard', mainController.postDashboard);
router.post('/logout', mainController.postLogout);

module.exports = router;